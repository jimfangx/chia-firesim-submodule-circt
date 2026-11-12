// See LICENSE for license details.
#include "goldengate/FAME5ThreadedState.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
std::string text(Operation *op) {
  std::string result; llvm::raw_string_ostream stream(result); op->print(stream); return result;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, StringRef path, unsigned threads = 4,
                            bool signedData = false) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  require(bool(buffer), "cannot read memory fixture");
  std::string input = buffer.get()->getBuffer().str();
  if (signedData)
    for (size_t pos = 0; (pos = input.find("UInt<8>", pos)) != std::string::npos; pos += 7)
      input.replace(pos, 7, "SInt<8>");
  auto pos = input.find("threadIdx : UInt<2>");
  input.replace(pos, 19, "threadIdx : UInt<" + std::to_string(llvm::Log2_64_Ceil(threads)) + ">");
  llvm::SourceMgr source;
  source.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(input), llvm::SMLoc());
  DefaultTimingManager timer; auto scope = timer.getRootScope();
  return importFIRFile(source, &context, scope, FIRParserOptions());
}
FModuleOp model(ModuleOp root) {
  auto circuit = *root.getOps<CircuitOp>().begin(); return *circuit.getOps<FModuleOp>().begin();
}
uint64_t evaluate(Value value, const llvm::DenseMap<Value, uint64_t> &values) {
  if (auto it = values.find(value); it != values.end()) return it->second;
  if (auto c = value.getDefiningOp<ConstantOp>()) return c.getValue().getZExtValue();
  if (auto c = value.getDefiningOp<CatPrimOp>())
    return (evaluate(c.getLhs(), values) << cast<UIntType>(c.getRhs().getType()).getWidth().value()) | evaluate(c.getRhs(), values);
  if (auto c = value.getDefiningOp<AddPrimOp>()) return evaluate(c.getLhs(), values) + evaluate(c.getRhs(), values);
  if (auto c = value.getDefiningOp<GEQPrimOp>()) return evaluate(c.getLhs(), values) >= evaluate(c.getRhs(), values);
  if (auto c = value.getDefiningOp<MuxPrimOp>()) return evaluate(evaluate(c.getSel(), values) ? c.getHigh() : c.getLow(), values);
  if (auto c = value.getDefiningOp<PadPrimOp>()) return evaluate(c.getInput(), values);
  if (auto c = value.getDefiningOp<BitsPrimOp>()) return (evaluate(c.getInput(), values) >> c.getLo()) & ((uint64_t(1) << (c.getHi()-c.getLo()+1))-1);
  throw std::runtime_error("unexpected address expression");
}
void behavior(MLIRContext &context, StringRef path, unsigned threads) {
  auto root = fixture(context, path, threads); require(bool(root), "memory fixture failed to import");
  auto mod = model(*root); auto mem = *mod.getOps<MemOp>().begin();
  auto attrs = mem->getAttrs();
  auto types = llvm::to_vector(mem.getResultTypes());
  Value index = mod.getBodyBlock()->getArgument(2);
  std::string error;
  require(succeeded(goldengate::threadFAME5Memories(mod, threads, index, error)), error.c_str());
  require(succeeded(verify(*root)), "threaded memory failed IR verification");
  mem = *mod.getOps<MemOp>().begin();
  require(mem.getDepth() == threads * 3 && mem.getReadLatency() == 0 && mem.getWriteLatency() == 1 && mem.getRuw() == RUWAttr::Old,
          "memory geometry, latency or read-under-write changed");
  for (auto attr : attrs)
    if (attr.getName() != "depth") require(mem->getAttr(attr.getName()) == attr.getValue(), "memory metadata changed");
  for (auto [oldType, newType] : llvm::zip(types, mem.getResultTypes())) {
    auto a = cast<BundleType>(oldType), b = cast<BundleType>(newType);
    require(a.getNumElements() == b.getNumElements(), "port field count changed");
    for (unsigned i = 0; i < a.getNumElements(); ++i)
      if (a.getElements()[i].name != "addr")
        require(a.getElements()[i] == b.getElements()[i], "nonaddress port type changed (data/mask/control)");
  }
  unsigned checked = 0;
  for (auto port : mem.getResults())
    for (Operation *user : port.getUsers()) {
      auto field = cast<SubfieldOp>(user);
      if (field.getFieldName() != "addr") continue;
      Operation *connect = *field.getResult().getUsers().begin();
      Value address = connect->getOperand(1);
      // Addresses are driven directly by one input in the original fixture.
      Value original;
      for (auto &op : *mod.getBodyBlock())
        if (auto cat = dyn_cast<CatPrimOp>(op); cat && cat.getResult() == address)
          original = cat.getLhs();
      Value base;
      if (threads == 3) {
        auto sum = address.getDefiningOp<AddPrimOp>();
        require(bool(sum), "missing non-power-of-two base-address mapping");
        base = sum.getLhs(); original = sum.getRhs();
        auto counter = base.getDefiningOp<RegOp>();
        require(counter && counter.getClockVal() == mod.getArgument(1), "accessor base is not an unreset host-clock register");
        Value next;
        for (auto &op : *mod.getBodyBlock())
          if (isa<ConnectOp, StrictConnectOp>(op) && op.getOperand(0) == base) next = op.getOperand(1);
        for (unsigned thread = 0; thread < threads; ++thread)
          require(evaluate(next, {{base, thread * 3}}) == ((thread + 1) % threads) * 3,
                  "non-power-of-two accessor base schedule changed");
      }
      require(bool(original), "missing threaded address mapping");
      for (unsigned thread = 0; thread < threads; ++thread)
        for (unsigned addr = 0; addr < 3; ++addr) {
          llvm::DenseMap<Value, uint64_t> values{{index, thread}, {original, addr}};
          if (base) values[base] = thread * 3;
          require(evaluate(address, values) == (base ? thread * 3 + addr : addr * threads + thread), "target memory aliases another thread");
          ++checked;
        }
    }
  auto before = text(*root);
  require(failed(goldengate::threadFAME5Memories(mod, threads, index, error)) && before == text(*root), "double threading did not reject atomically");
  llvm::outs() << "Passed " << threads << "-thread geometry/mask/readwriter/RUW preservation and " << checked << " translated addresses\n";
}
void rejections(MLIRContext &context, StringRef path) {
  for (unsigned test = 0; test < 7; ++test) {
    auto root = fixture(context, path); auto mod = model(*root); auto mem = *mod.getOps<MemOp>().begin();
    OpBuilder b(&context);
    if (test == 0) {
      auto names = llvm::to_vector(mod.getPortNames()); names[1] = b.getStringAttr("unrelatedClock");
      mod->setAttr("portNames", b.getArrayAttr(names));
    }
    if (test == 1) mem.setReadLatencyAttr(b.getI32IntegerAttr(1));
    if (test == 2) mem.setWriteLatencyAttr(b.getI32IntegerAttr(2));
    if (test == 3) mem->setAttr("annotations", b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("Unknown"))})}));
    if (test == 4) mod->getParentOfType<CircuitOp>()->setAttr("rawAnnotations", b.getArrayAttr({b.getStringAttr("~Model|Top/inst:Model>ram.r.data.lo")}));
    if (test == 5) mem->setAttr("init", b.getUnitAttr());
    if (test == 6) {
      for (Operation *user : mem.getResult(0).getUsers())
        if (auto field = dyn_cast<SubfieldOp>(user); field && field.getFieldName() == "addr") {
          Operation *connect = *field.getResult().getUsers().begin();
          OpBuilder clone(connect); clone.clone(*connect); break;
        }
    }
    auto before = text(*root); std::string error;
    require(failed(goldengate::threadFAME5Memories(mod, test == 0 ? 3 : 4, mod.getBodyBlock()->getArgument(2), error)) && !error.empty(), "unsupported memory accepted");
    require(before == text(*root), "unsupported memory mutated module");
  }
  llvm::outs() << "Passed seven atomic unsupported-memory rejection cases\n";
}
void synchronous(MLIRContext &context, StringRef path, unsigned threads, bool signedData = false) {
  auto root = fixture(context, path, threads, signedData);
  require(bool(root), "synchronous fixture failed to import");
  auto mod = model(*root);
  std::string error;
  require(succeeded(goldengate::threadFAME5Memories(mod, threads,
      mod.getBodyBlock()->getArgument(2), error)), error.c_str());
  require(succeeded(verify(*root)), "synchronous threading failed IR verification");
  unsigned targets = 0, responses = 0;
  for (auto mem : mod.getOps<MemOp>()) {
    require(mem.getDataType() == (signedData ? FIRRTLBaseType(SIntType::get(&context, 8)) : FIRRTLBaseType(UIntType::get(&context, 8))),
            "target/response memory changed data signedness or width");
    if (mem->hasAttr("goldengate.fame5.responseBuffer")) {
      require(mem.getDepth() == threads && mem.getReadLatency() == (threads < 4 ? 0 : 1) &&
              mem.getWriteLatency() == 1, "response-bank geometry/latency mismatch");
      require(mem.getRuw() == RUWAttr::Undefined, "response-bank RUW changed from SFC");
      ++responses;
    } else {
      require(mem.getDepth() == 3 * threads && mem.getReadLatency() == 1 &&
              mem.getWriteLatency() == 1 && mem.getRuw() == RUWAttr::Old,
              "synchronous target memory semantics changed");
      ++targets;
    }
  }
  require(targets == 1 && responses == 2, "reader/readwriter responses do not have independent banks");
  unsigned targetRegs = 0, hostRegs = 0;
  for (auto reg : mod.getOps<RegOp>()) {
    if (reg.getClockVal() == mod.getBodyBlock()->getArgument(0)) ++targetRegs;
    else if (reg.getClockVal() == mod.getBodyBlock()->getArgument(1)) ++hostRegs;
    else require(false, "response state uses an unrelated clock");
  }
  require(targetRegs == 1 && hostRegs == (threads < 4 ? 3 : 8) + (threads == 3 ? 3 : 0),
          "target-edge detector or host response pipeline is incomplete");
  llvm::outs() << "Passed " << threads << "-thread " << (signedData ? "signed" : "unsigned") << " synchronous reader/readwriter response banks and clock/latency schedule\n";
}
void synchronousRejections(MLIRContext &context, StringRef path) {
  for (unsigned test = 0; test < 6; ++test) {
    auto root = fixture(context, path, 4); auto mod = model(*root);
    auto mem = *mod.getOps<MemOp>().begin(); OpBuilder b(&context);
    if (test == 0) mem.setReadLatencyAttr(b.getI32IntegerAttr(2));
    if (test == 1) {
      auto names = llvm::to_vector(mod.getPortNames());
      names[1] = b.getStringAttr("unrelatedClock");
      mod->setAttr("portNames", b.getArrayAttr(names));
    }
    if (test == 2 || test == 3) {
      for (Operation *user : mem.getResult(1).getUsers())
        if (auto field = dyn_cast<SubfieldOp>(user); field && field.getFieldName() == "clk") {
          auto driver = *field.getResult().getUsers().begin();
          if (test == 2) driver->setOperand(1, mod.getBodyBlock()->getArgument(1));
          else { OpBuilder clone(driver); clone.clone(*driver); }
          break;
        }
    }
    if (test == 4 || test == 5) {
      auto wire = *mod.getOps<WireOp>().begin();
      for (auto &use : wire.getResult().getUses())
        if (isa<ConnectOp, StrictConnectOp>(use.getOwner()) && use.getOperandNumber() == 0) {
          if (test == 4) use.getOwner()->setOperand(1, wire.getResult());
          else { OpBuilder clone(use.getOwner()); clone.clone(*use.getOwner()); }
          break;
        }
    }
    auto before = text(*root); std::string error;
    require(failed(goldengate::threadFAME5Memories(mod, 4,
      mod.getBodyBlock()->getArgument(2), error)), "unsupported synchronous clock/latency accepted");
    require(before == text(*root), "unsupported synchronous memory mutated module");
  }
  llvm::outs() << "Passed six atomic synchronous-memory rejection cases\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: FAME5ThreadedMemoryTest fixture.fir");
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context, argv[1], 2); behavior(context, argv[1], 3); behavior(context, argv[1], 4); behavior(context, argv[1], 64); rejections(context, argv[1]);
    llvm::SmallString<256> syncPath(argv[1]);
    llvm::sys::path::remove_filename(syncPath);
    llvm::sys::path::append(syncPath, "FAME5SynchronousMemory.fir");
    synchronous(context, syncPath, 2); synchronous(context, syncPath, 3); synchronous(context, syncPath, 4); synchronous(context, syncPath, 64);
    synchronous(context, syncPath, 2, true); synchronous(context, syncPath, 3, true); synchronous(context, syncPath, 4, true); synchronous(context, syncPath, 64, true);
    synchronousRejections(context, syncPath); return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
