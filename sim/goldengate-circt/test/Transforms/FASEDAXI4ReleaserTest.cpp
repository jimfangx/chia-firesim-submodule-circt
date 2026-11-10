// See LICENSE for license details.
#include "goldengate/FASEDAXI4Releaser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <random>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, StringRef why) {
  if (!ok)
    throw std::runtime_error(why.str());
}
std::string print(Operation *op) {
  std::string text;
  llvm::raw_string_ostream os(text);
  op->print(os);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }", &ctx);
  require(bool(root), "fixture parse failed");
  return root;
}
// Execute the materialized FIRRTL rather than a handwritten implementation.
struct Evaluator {
  FModuleOp module;
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, APInt> state, memo;
  explicit Evaluator(FModuleOp m) : module(m) {
    for (auto c : module.getOps<StrictConnectOp>())
      require(drivers.try_emplace(c.getDest(), c.getSrc()).second,
              "multiple drivers");
    for (auto r : module.getOps<RegResetOp>())
      state[r.getResult()] = APInt(1, 0);
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  void put(unsigned i, uint64_t value) {
    memo[arg(i)] = APInt(width(arg(i)), value);
  }
  APInt eval(Value v) {
    if (auto it = memo.find(v); it != memo.end())
      return it->second;
    auto *op = v.getDefiningOp();
    APInt value(width(v), 0);
    if (isa_and_nonnull<RegResetOp>(op))
      value = state.lookup(v);
    else if (auto it = drivers.find(v); it != drivers.end())
      value = eval(it->second);
    else if (auto c = dyn_cast_or_null<ConstantOp>(op))
      value = c.getValue();
    else if (isa_and_nonnull<MuxPrimOp>(op))
      value = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (isa_and_nonnull<AndPrimOp>(op))
      value = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op))
      value = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op))
      value = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op))
      value = ~eval(op->getOperand(0));
    else
      throw std::runtime_error("unsupported operation or missing input");
    memo[v] = value;
    return value;
  }
  uint64_t get(unsigned i) { return eval(arg(i)).getZExtValue(); }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero()
                                ? eval(drivers.lookup(r.getResult()))
                                : eval(r.getResetValue());
    state = std::move(next);
  }
};
void behavior(FModuleOp module) {
  Evaluator sim(module);
  Value readFull, writeFull;
  for (auto reg : module.getOps<RegResetOp>()) {
    if (reg.getName() == "currentRead_full")
      readFull = reg.getResult();
    else if (reg.getName() == "currentWrite_full")
      writeFull = reg.getResult();
    else
      throw std::runtime_error("unexpected metadata state");
  }
  require(sim.state.size() == 2 && readFull && writeFull &&
              module.getOps<RegOp>().empty(),
          "releaser must store only two occupancy bits");
  require(module.getNumPorts() == 36, "unexpected scalar interface");
  std::mt19937_64 random(0x510ce);
  unsigned cases = 0, replacements = 0, stalledReset = 0, multibeat = 0,
           emptyPush = 0, blocked = 0, held = 0;
  auto sample = [&](bool rf, bool wf, unsigned flags, unsigned id) {
    sim.memo.clear();
    sim.state[readFull] = APInt(1, rf);
    sim.state[writeFull] = APInt(1, wf);
    bool reset = flags & 1, fire = flags >> 1 & 1, nr = flags >> 2 & 1,
         nw = flags >> 3 & 1, rr = flags >> 4 & 1, br = flags >> 5 & 1,
         last = flags >> 6 & 1;
    sim.put(1, reset);
    sim.put(2, fire);
    sim.put(3, nr);
    sim.put(5, id);
    sim.put(6, random() & 255);
    sim.put(7, nw);
    sim.put(9, 15 - id);
    sim.put(10, rr);
    sim.put(17, br);
    uint64_t payload[] = {random() & 15,
                          last,
                          random(),
                          random() & 3,
                          random() & 1,
                          random() & 15,
                          random() & 3,
                          random() & 1};
    for (unsigned i = 0; i < 8; ++i)
      sim.put(26 + i, payload[i]);
    bool rpop = rf && rr && last, bpop = wf && br;
    bool rready = !rf || rpop, bready = !wf || bpop;
    bool rpush = nr && rready, bpush = nw && bready;
    require(sim.get(4) == rready && sim.get(8) == bready,
            "one-entry pipe capacity differs");
    require(sim.get(11) == rf && sim.get(18) == wf,
            "empty queue incorrectly flows or response occupancy differs");
    require(sim.get(22) == rpush && sim.get(24) == bpush,
            "egress request must follow accepted metadata combinationally");
    require(sim.get(23) == id && sim.get(25) == 15 - id,
            "egress request IDs changed even while invalid");
    require(sim.get(34) == rr && sim.get(35) == br,
            "egress ready must forward target ready unconditionally");
    for (auto [dest, src] : {std::pair{12u, 0u},
                             {13u, 1u},
                             {14u, 2u},
                             {15u, 3u},
                             {16u, 4u},
                             {19u, 5u},
                             {20u, 6u},
                             {21u, 7u}})
      require(sim.get(dest) == payload[src],
              "egress payload changed or was gated while invalid");
    sim.edge();
    bool nextRead = fire ? (!reset && (rpush || (rf && !rpop))) : rf;
    bool nextWrite = fire ? (!reset && (bpush || (wf && !bpop))) : wf;
    require(sim.state.lookup(readFull).getBoolValue() == nextRead &&
                sim.state.lookup(writeFull).getBoolValue() == nextWrite,
            "occupancy target edge/reset/replacement differs");
    ++cases;
    replacements += fire && rpop && rpush;
    stalledReset += reset && !fire && (rf || wf);
    multibeat += rf && rr && !last;
    emptyPush += !rf && rpush;
    blocked += nr && !rready;
    held += !fire && (rpop || rpush || bpop || bpush);
  };
  for (unsigned rf = 0; rf < 2; ++rf)
    for (unsigned wf = 0; wf < 2; ++wf)
      for (unsigned flags = 0; flags < 128; ++flags)
        for (unsigned id = 0; id < 16; ++id)
          sample(rf, wf, flags, id);
  for (unsigned i = 0; i < 24000; ++i)
    sample(sim.state.lookup(readFull).getBoolValue(),
           sim.state.lookup(writeFull).getBoolValue(),
           random() & 127,
           random() & 15);
  require(replacements && stalledReset && multibeat && emptyPush && blocked &&
              held,
          "missing releaser coverage");
  llvm::outs() << cases
               << " AXI4 releaser transitions; replacements=" << replacements
               << ", stalled-reset=" << stalledReset
               << ", non-last=" << multibeat << ", empty-push=" << emptyPush
               << ", blocked=" << blocked << ", held=" << held << "\n";
}
void collisions(MLIRContext &ctx, FModuleOp sentinel) {
  for (bool external : {false, true}) {
    std::string text =
        "module { firrtl.circuit \"Top\" { firrtl.module @Top() {} ";
    text += external ? "firrtl.extmodule @GGFASEDAXI4Releaser() } }"
                     : "firrtl.module @GGFASEDAXI4Releaser() {} } }";
    auto root = parseSourceString<ModuleOp>(text, &ctx);
    require(bool(root), "collision fixture invalid");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto before = print(root->getOperation());
    auto result = sentinel;
    std::string error;
    require(failed(goldengate::materializeFASEDAXI4Releaser(
                circuit, result, error)) &&
                !error.empty(),
            "helper symbol collision accepted");
    require(print(root->getOperation()) == before && result == sentinel,
            "symbol rejection mutated circuit or result");
  }
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc <= 2, "usage: test [output.mlir]");
    MLIRContext ctx;
    ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx);
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto top = *circuit.getOps<FModuleOp>().begin();
    auto before = print(top);
    FModuleOp helper;
    std::string error;
    require(succeeded(goldengate::materializeFASEDAXI4Releaser(
                circuit, helper, error)),
            error);
    require(succeeded(verify(*root)), "materialized releaser IR invalid");
    require(print(top) == before && circuit.getName() == "Top",
            "materialization modified original top");
    behavior(helper);
    collisions(ctx, helper);
    before = print(root->getOperation());
    auto result = helper;
    require(failed(goldengate::materializeFASEDAXI4Releaser(
                circuit, result, error)),
            "repeat materialization accepted");
    require(print(root->getOperation()) == before && result == helper,
            "repeat rejection mutated circuit or result");
    if (argc == 2) {
      circuit.setName(helper.getName());
      std::error_code ec;
      llvm::raw_fd_ostream out(argv[1], ec);
      require(!ec, "cannot export fixture");
      root->print(out);
      out << '\n';
    }
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
