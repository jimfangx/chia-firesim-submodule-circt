// See LICENSE for license details.
#include "goldengate/FAME5ThreadedState.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string text(Operation *op) {
  std::string result;
  llvm::raw_string_ostream stream(result);
  op->print(stream);
  return result;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned count) {
  std::string ir = R"mlir(module { firrtl.circuit "Model" {
    firrtl.module @Model(in %clock: !firrtl.clock, in %thread: !firrtl.uint<INDEX>,
        in %reset: !firrtl.uint<1>, in %enable: !firrtl.uint<1>,
        in %data: !firrtl.uint<8>, out %result: !firrtl.uint<8>) {
      %c7 = firrtl.constant 7 : !firrtl.uint<8>
      %state = firrtl.regreset %clock, %reset, %c7 : !firrtl.clock, !firrtl.uint<1>, !firrtl.uint<8>, !firrtl.uint<8>
      %next = firrtl.mux(%enable, %data, %state) : (!firrtl.uint<1>, !firrtl.uint<8>, !firrtl.uint<8>) -> !firrtl.uint<8>
      firrtl.strictconnect %state, %next : !firrtl.uint<8>
      firrtl.strictconnect %result, %state : !firrtl.uint<8>
    }
  } })mlir";
  auto pos = ir.find("INDEX");
  ir.replace(pos, 5, std::to_string(llvm::Log2_64_Ceil(count)));
  return parseSourceString<ModuleOp>(ir, &ctx);
}
FModuleOp model(ModuleOp root) {
  auto circuit = *root.getOps<CircuitOp>().begin();
  return *circuit.getOps<FModuleOp>().begin();
}
unsigned eval(Value value, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto it = values.find(value); it != values.end())
    return it->second;
  if (auto op = value.getDefiningOp<ConstantOp>())
    return op.getValue().getZExtValue();
  if (auto op = value.getDefiningOp<MuxPrimOp>())
    return eval(op.getSel(), values) ? eval(op.getHigh(), values) : eval(op.getLow(), values);
  if (auto op = value.getDefiningOp<LTPrimOp>())
    return eval(op.getLhs(), values) < eval(op.getRhs(), values);
  if (auto op = value.getDefiningOp<SubfieldOp>()) {
    for (Operation *user : value.getUsers())
      if (isa<ConnectOp, StrictConnectOp>(user) && user->getOperand(0) == value)
        return eval(user->getOperand(1), values);
  }
  throw std::runtime_error("unhandled state expression");
}
Value field(FModuleOp mod, Value port, StringRef name) {
  for (auto op : mod.getOps<SubfieldOp>())
    if (op.getInput() == port && op.getFieldName() == name)
      return op.getResult();
  throw std::runtime_error("missing memory field");
}
void behavior(MLIRContext &ctx, unsigned count) {
  auto root = fixture(ctx, count);
  require(bool(root), "fixture did not parse");
  auto mod = model(*root);
  auto arg = [&](unsigned index) { return mod.getBodyBlock()->getArgument(index); };
  std::string error;
  require(succeeded(goldengate::threadFAME5Registers(mod, count, arg(1), error)), error.c_str());
  require(succeeded(verify(*root)), "threaded state IR does not verify");
  require(mod.getOps<RegOp>().empty() && mod.getOps<RegResetOp>().empty(), "unthreaded register remains");
  auto memories = mod.getOps<MemOp>();
  require(std::distance(memories.begin(), memories.end()) == 1, "wrong number of banks");
  auto mem = *memories.begin();
  require(mem.getDepth() == count && mem.getReadLatency() == 0 && mem.getWriteLatency() == 1,
          "wrong state bank geometry/latency");
  Value read = mem.getResult(0), write = mem.getResult(1);
  require(eval(field(mod, write, "mask"), llvm::DenseMap<Value, unsigned>()) == 1, "state write mask changed");
  // This executes emitted memory addresses/write-data expressions against a
  // replicated-register machine. Each active slot has its own reset/enable.
  // Reset cycles and stalled slots must not disturb any other thread.
  SmallVector<unsigned> actual(count), expected(count);
  for (unsigned thread = 0; thread < count; ++thread)
    actual[thread] = expected[thread] = 31 + thread;
  unsigned observations = 0;
  uint64_t checksum = 0;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    unsigned thread = (cycle * 7 + cycle / 13) % count;
    unsigned reset = cycle % 37 == 0;
    unsigned enable = (cycle + thread) % 5 != 0;
    unsigned data = (cycle * 17 + thread * 29) & 255;
    llvm::DenseMap<Value, unsigned> values{{arg(1), thread}, {arg(2), reset},
        {arg(3), enable}, {arg(4), data}, {field(mod, read, "data"), actual[thread]}};
    unsigned raddr = eval(field(mod, read, "addr"), values);
    unsigned waddr = eval(field(mod, write, "addr"), values);
    require(raddr == thread && waddr == thread, "selected slot address changed");
    unsigned next = eval(field(mod, write, "data"), values);
    if (eval(field(mod, write, "en"), values))
      actual[waddr] = next;
    expected[thread] = reset ? 7 : enable ? data : expected[thread];
    require(actual == expected, "thread reset/hold/state isolation differs from replicated registers");
    for (unsigned slot = 0; slot < count; ++slot)
      checksum = checksum * 131 + actual[slot] + slot;
    ++observations;
  }
  if (count == 3) {
    llvm::DenseMap<Value, unsigned> invalid{{arg(1), 3}};
    require(eval(field(mod, write, "en"), invalid) == 0, "invalid thread index writes storage");
  }
  auto before = text(*root);
  require(failed(goldengate::threadFAME5Registers(mod, count, arg(1), error)) && before == text(*root),
          "double threading did not reject atomically");
  llvm::outs() << "Passed " << count << "-thread state/reset/stall isolation: " << observations << " edges\n";
  llvm::outs() << "ORACLE " << count << " " << checksum << "\n";
}
void rejections(MLIRContext &ctx) {
  for (unsigned test = 0; test < 7; ++test) {
    auto root = fixture(ctx, 2);
    auto mod = model(*root);
    auto index = mod.getBodyBlock()->getArgument(test == 0 ? 4 : 1);
    auto reg = *mod.getOps<RegResetOp>().begin();
    if (test == 1) {
      OpBuilder b(reg);
      reg.getResetSignalMutable().assign(b.create<AsAsyncResetPrimOp>(reg.getLoc(), mod.getBodyBlock()->getArgument(2)));
    }
    if (test == 2) {
      for (auto op : llvm::make_early_inc_range(mod.getOps<StrictConnectOp>()))
        if (op.getDest() == reg.getResult())
          op.erase();
    }
    if (test == 3) {
      OpBuilder b(&mod.getBodyBlock()->back());
      b.create<StrictConnectOp>(reg.getLoc(), reg.getResult(), reg.getResult());
    }
    if (test == 4) {
      OpBuilder b(&ctx);
      reg->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    }
    if (test == 5 || test == 6) {
      OpBuilder b(&ctx);
      auto circuit = mod->getParentOfType<CircuitOp>();
      auto target = test == 5 ? "~Model|Model>state" : "~Model|Parent/child:Model>state.field";
      circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("target", b.getStringAttr(target))})}));
    }
    auto before = text(*root);
    std::string error;
    require(failed(goldengate::threadFAME5Registers(mod, 2, index, error)) && !error.empty(), "unsupported state accepted");
    require(before == text(*root), "invalid state partially mutated module");
  }
  llvm::outs() << "Passed seven atomic unsupported-state rejection cases\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (unsigned count : {2u, 3u, 4u, 64u})
      behavior(context, count);
    rejections(context);
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n';
    return 1;
  }
}
