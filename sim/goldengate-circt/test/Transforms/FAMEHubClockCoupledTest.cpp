// See LICENSE for license details.
// Couple the emitted rational producer to the emitted FAME hub controls.
// Evaluate FIRRTL connects, simultaneous host state and actual gate enables;
// compare the trace with FAMEHubClockCoupledOracle.scala.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "goldengate/RationalClockTokenGenerator.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include <map>
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
Value field(OpBuilder &b, Location loc, Value port, StringRef name) {
  return b.create<SubfieldOp>(loc, port, name);
}
// Canonical field identities bridge model arguments to their actual instance
// results, so ready feeds the producer through the constructed FIRRTL graph.
struct Interpreter {
  FModuleOp top, model;
  llvm::DenseMap<Value, Value> aliases;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  std::set<std::string> visiting;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>())
      return key(f.getInput()) + "." + f.getFieldName().str();
    if (aliases.count(v)) return key(aliases.lookup(v));
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp t, FModuleOp m, InstanceOp instance) : top(t), model(m) {
    for (unsigned i = 0; i < m.getNumPorts(); ++i)
      aliases[m.getArgument(i)] = instance.getResult(i);
    for (auto scope : {top, model})
      for (auto c : scope.getOps<StrictConnectOp>())
        require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
  }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    require(visiting.insert(k).second, "combinational loop");
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AsUIntPrimOp, AsClockPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (auto p = dyn_cast_or_null<MuxPrimOp>(op)) n = eval(eval(p.getSel()) ? p.getHigh() : p.getLow());
    else if (auto p = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(p.getInput()) >> p.getLo();
    else throw std::runtime_error("unsupported operation or missing driver: " + k);
    if (auto type = dyn_cast<UIntType>(v.getType()); type && type.getWidth() && *type.getWidth() < 64)
      n &= (uint64_t(1) << *type.getWidth()) - 1;
    visiting.erase(k); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto scope : {top, model}) {
      for (auto r : scope.getOps<RegResetOp>())
        next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
      for (auto r : scope.getOps<RegOp>()) {
        // Target registers are clocked by the actual AbstractClockGate.O.
        auto clock = dyn_cast<OpResult>(r.getClockVal());
        auto gate = clock ? dyn_cast<InstanceOp>(clock.getOwner()) : InstanceOp();
        require(gate && clock.getResultNumber() == 2, "target state lacks gated clock");
        next[r.getResult()] = eval(gate.getResult(1)) ? eval(drivers.at(key(r.getResult()))) : state.lookup(r.getResult());
      }
    }
    state = std::move(next);
  }
};

void clockRecordRejections(MLIRContext &ctx) {
  for (StringRef payload : {"uint<1>", "bundle<>",
                           "bundle<_0: clock, data: uint<1>>",
                           "bundle<_0 flip: clock>",
                           "bundle<nested: bundle<_0: clock>>",
                           "vector<clock, 2>",
                           "bundle<_0: clock, _1: clock>"}) {
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Model\" { firrtl.module @Model("
        "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
        "in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>, "
        "in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: " +
        payload.str() + ">) { %done = firrtl.wire : !firrtl.uint<1> } } }", &ctx);
    require(bool(root), "clock rejection fixture parse");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    for (auto w : model.getOps<WireOp>()) w.setName("targetCycleFinishing");
    auto before = dump(root->getOperation());
    std::string error;
    // The final case is a valid ClockRecord wrongly listed as a data input.
    bool isRecord = payload == "bundle<_0: clock, _1: clock>";
    SmallVector<std::string> inputs = isRecord ?
        SmallVector<std::string>{"tick", "in0"} : SmallVector<std::string>{"in0"};
    require(failed(goldengate::rewriteFAMEFinishing(model, inputs, {},
                    isRecord ? "" : "tick", error)) &&
                error.find(isRecord ? "explicit target clock" : "Clock/ClockRecord") != std::string::npos &&
                before == dump(root->getOperation()),
            "clock payload was not rejected before mutation: " + payload.str());
  }
  llvm::errs() << "Passed seven atomic ClockRecord completion rejections\n";
}

void outputValidRejections(MLIRContext &ctx) {
  // HasModelPort constructs passive valid fields; a flipped field is driven
  // by the peer. Reject it before even a preceding valid channel is rewritten.
  for (bool flippedInput : {false, true}) {
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"Model\" { firrtl.module @Model("
        "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, "
        "in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid" +
        std::string(flippedInput ? " flip" : "") + ": uint<1>, bits: uint<16>>, "
        "out %out0_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>, "
        "out %out1_source: !firrtl.bundle<ready flip: uint<1>, valid" +
        std::string(flippedInput ? "" : " flip") + ": uint<1>, bits: uint<16>>) { "
        "%done = firrtl.wire : !firrtl.uint<1> } } }", &ctx);
    require(bool(root), "output valid rejection fixture parse");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto model = *circuit.getOps<FModuleOp>().begin();
    for (auto w : model.getOps<WireOp>()) w.setName("targetCycleFinishing");
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(model,
            {{"out0", false, {}}, {"out1", false, {}}}, error)), error);
    auto before = dump(root->getOperation());
    require(failed(goldengate::rewriteFAMEOutputValids(model,
            {{"out0", {}, {}, {}}, {"out1", {"in0"}, {}, {}}}, error)) &&
            error.find("valid port") != std::string::npos &&
            before == dump(root->getOperation()),
            "flipped valid field accepted or partially rewritten");
  }
  llvm::errs() << "Passed two atomic flipped-valid rejections\n";
}

void run(MLIRContext &ctx, bool reversed, const char *output) {
  std::string common = R"mlir(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
    in %in0_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>,
    in %in1_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>,
    out %out0_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>,
    out %out1_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>)mlir";
  auto payload = reversed ? "_1: clock, _0: clock" : "_0: clock, _1: clock";
  auto root = parseSourceString<ModuleOp>(
    "module { firrtl.circuit \"Top\" { firrtl.module @Top(" + common + ") {} "
    "firrtl.module private @Model(" + common +
    ", in %bridge_clocks_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<" + payload + ">>) {\n" +
    R"mlir(%targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      %bits = firrtl.subfield %bridge_clocks_sink[bits] : !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<)mlir" + payload + R"mlir(>>
      %clock0 = firrtl.subfield %bits[_0] : !firrtl.bundle<)mlir" + payload + R"mlir(>
      %clock1 = firrtl.subfield %bits[_1] : !firrtl.bundle<)mlir" + payload + R"mlir(>
      %state0 = firrtl.reg %clock0 : !firrtl.clock, !firrtl.uint<16>
      %state1 = firrtl.reg %clock1 : !firrtl.clock, !firrtl.uint<16>
      %input0 = firrtl.subfield %in0_sink[bits] : !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>
      %input1 = firrtl.subfield %in1_sink[bits] : !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<16>>
      %sum0 = firrtl.add %state0, %input0 : (!firrtl.uint<16>, !firrtl.uint<16>) -> !firrtl.uint<17>
      %sum1 = firrtl.add %state1, %input1 : (!firrtl.uint<16>, !firrtl.uint<16>) -> !firrtl.uint<17>
      %next0 = firrtl.bits %sum0 15 to 0 : (!firrtl.uint<17>) -> !firrtl.uint<16>
      %next1 = firrtl.bits %sum1 15 to 0 : (!firrtl.uint<17>) -> !firrtl.uint<16>
      firrtl.strictconnect %state0, %next0 : !firrtl.uint<16>
      firrtl.strictconnect %state1, %next1 : !firrtl.uint<16>
    } } })mlir", &ctx);
  require(bool(root), "coupled fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  auto loc = model.getLoc(); OpBuilder b(&ctx); std::string error;
  b.setInsertionPointToStart(model.getBodyBlock());
  auto bits = field(b, loc, model.getArgument(6), "bits");
  SmallVector<Value> raw(2), enabled(2), targetState;
  for (unsigned i = 0; i < 2; ++i) {
    auto name = "bridge_clocks_" + std::to_string(i);
    auto token = field(b, loc, bits, "_" + std::to_string(i));
    raw[i] = b.create<AsUIntPrimOp>(loc, token);
    require(succeeded(goldengate::addFAMEClockEnable(model, name, raw[i], error)), error);
    require(succeeded(goldengate::addFAMEClockGate(circuit, model, name, error, token)), error);
    enabled[i] = goldengate::lookupFAMEClockEnable(model, name, error);
  }
  SmallVector<goldengate::FAMEFiredChannel> channels{
    {"in0", true, raw[0]}, {"in1", true, raw[1]},
    {"out0", false, enabled[0]}, {"out1", false, enabled[1]}};
  require(succeeded(goldengate::ensureFAMEFiredRegisters(model, channels, error)), error);
  require(succeeded(goldengate::rewriteFAMEFiredStates(model, channels, error)), error);
  require(succeeded(goldengate::rewriteFAMEOutputValids(model,
              {{"out0", {"in0"}, {}, {}}, {"out1", {"in1"}, {}, {}}}, error)), error);
  require(succeeded(goldengate::rewriteFAMEInputReadies(model, {"in0", "in1"}, error)), error);
  require(succeeded(goldengate::rewriteFAMEFinishing(model,
              {"in0", "in1"}, {"out0", "out1"}, "bridge_clocks", error)), error);
  b.setInsertionPointToEnd(model.getBodyBlock());
  for (auto r : model.getOps<RegOp>()) targetState.push_back(r.getResult());
  require(targetState.size() == 2, "target state missing");
  for (unsigned i = 0; i < 2; ++i) {
    auto sum = b.create<AddPrimOp>(loc, targetState[i],
        field(b, loc, model.getArgument(2 + i), "bits"));
    auto data = b.create<BitsPrimOp>(loc, sum, 15, 0);
    b.create<StrictConnectOp>(loc, field(b, loc, model.getArgument(4 + i), "bits"), data);
  }

  b.setInsertionPointToEnd(top.getBodyBlock());
  auto instance = b.create<InstanceOp>(loc, model, "model");
  for (unsigned i = 0; i < 2; ++i)
    b.create<StrictConnectOp>(loc, instance.getResult(i), top.getArgument(i));
  for (unsigned i = 2; i < 6; ++i)
    for (StringRef member : {"ready", "valid", "bits"}) {
      auto outer = field(b, loc, top.getArgument(i), member);
      auto inner = field(b, loc, instance.getResult(i), member);
      bool intoModel = (i < 4) != (member == "ready");
      b.create<StrictConnectOp>(loc, intoModel ? inner : outer, intoModel ? outer : inner);
    }
  auto clockPort = instance.getResult(6);
  auto ready = field(b, loc, clockPort, "ready");
  auto schedule = goldengate::analyzeRationalClockSchedule(
      {{"domain0", 1, 2, 2}, {"domain1", 1, 3, 3}}, error);
  require(bool(schedule), error);
  auto tokens = goldengate::buildRationalClockTokens(b, loc,
      top.getArgument(0), top.getArgument(1), ready, *schedule);
  auto one = b.create<ConstantOp>(loc, UIntType::get(&ctx, 1), APInt(1, 1));
  b.create<StrictConnectOp>(loc, field(b, loc, clockPort, "valid"), one.getResult());
  auto instanceBits = field(b, loc, clockPort, "bits");
  for (unsigned lane = 0; lane < 2; ++lane) {
    auto token = b.create<AsClockPrimOp>(loc, tokens[lane]);
    b.create<StrictConnectOp>(loc, field(b, loc, instanceBits,
        "_" + std::to_string(reversed ? 1 - lane : lane)), token.getResult());
  }
  require(succeeded(verify(*root)), "coupled FIRRTL verification");
  if (output) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, ec.message()); root->print(out); out << '\n';
  }

  Interpreter sim(top, model, instance);
  goldengate::FAMEClockGateIndex gates;
  goldengate::FAMEFiredRegisterIndex fired;
  require(succeeded(gates.collect(model, error)) && succeeded(fired.collect(model, error)), error);
  Value finishing;
  for (auto w : model.getOps<WireOp>())
    if (w.getName() == "targetCycleFinishing") finishing = w.getResult();
  bool pending[2] = {false, false};
  uint64_t inputBits[2] = {0, 0}, expectedState[2] = {0, 0};
  bool blocked[2] = {false, false};
  uint64_t blockedBits[2] = {0, 0};
  uint64_t edges[2] = {0, 0}, outputCount[2] = {0, 0}, inputCount[2] = {0, 0};
  unsigned stalls = 0, unequal = 0, completed = 0;
  for (unsigned cycle = 0; cycle < 8192; ++cycle) {
    bool reset = cycle < 3 || cycle == 4096 || cycle == 4097;
    if (reset) {
      pending[0] = pending[1] = false;
      inputBits[0] = inputBits[1] = 0;
    }
    else {
      if (!pending[0] && (cycle % 11 == 3 || cycle % 43 > 37)) {
        pending[0] = true; inputBits[0] = (cycle * 73 + 19) & 65535;
      }
      if (!pending[1] && (cycle % 17 == 5 || cycle % 61 > 53)) {
        pending[1] = true; inputBits[1] = (cycle * 151 + 41) & 65535;
      }
    }
    sim.memo.clear();
    sim.memo[sim.key(top.getArgument(0))] = 0;
    sim.memo[sim.key(top.getArgument(1))] = reset;
    for (unsigned i = 0; i < 2; ++i) {
      sim.memo[sim.key(top.getArgument(2 + i)) + ".valid"] = pending[i];
      sim.memo[sim.key(top.getArgument(2 + i)) + ".bits"] = inputBits[i];
      sim.memo[sim.key(top.getArgument(4 + i)) + ".ready"] = i == 0 ?
          cycle % 97 >= 29 && cycle % 7 != 0 : cycle % 83 >= 37 && cycle % 11 != 0;
    }
    auto evalField = [&](unsigned port, StringRef member) {
      auto k = sim.key(top.getArgument(port)) + "." + member.str();
      return sim.memo.count(k) ? sim.memo.at(k) : sim.eval(sim.drivers.at(k));
    };
    unsigned mask = sim.eval(tokens[0]) | sim.eval(tokens[1]) << 1;
    unsigned done = sim.eval(finishing), enableMask = 0, firedMask = 0, inReady = 0, outValid = 0, ceMask = 0;
    for (unsigned i = 0; i < 2; ++i) {
      enableMask |= sim.eval(enabled[i]) << i;
      inReady |= evalField(2 + i, "ready") << i;
      outValid |= evalField(4 + i, "valid") << i;
      ceMask |= sim.eval(gates.lookup("bridge_clocks_" + std::to_string(i), false).getResult(1)) << i;
    }
    for (unsigned i = 0; i < channels.size(); ++i)
      firedMask |= sim.eval(fired.lookup(channels[i].name)) << i;
    llvm::outs() << "TRACE " << cycle << ' ' << mask << ' ' << done << ' ' << enableMask << ' '
      << firedMask << ' ' << inReady << ' ' << outValid << ' ' << ceMask << ' '
      << sim.eval(targetState[0]) << ' ' << sim.eval(targetState[1]) << ' '
      << evalField(4, "bits") << ' ' << evalField(5, "bits") << '\n';
    require(sim.eval(targetState[0]) == expectedState[0] &&
            sim.eval(targetState[1]) == expectedState[1], "target-cycle state trajectory differs");
    require(!reset || !ceMask, "host reset advanced target state");
    if (!done && !reset) ++stalls;
    if (enableMask == 1 || enableMask == 2) ++unequal;
    if (done && !reset) ++completed;
    auto heldMask = mask;
    // Observe each actual output handshake, including early independent firing.
    for (unsigned i = 0; i < 2; ++i) {
      auto data = evalField(4 + i, "bits");
      require(data == ((expectedState[i] + inputBits[i]) & 65535),
              "output payload does not reflect target state and current input");
      if (!reset && blocked[i])
        require((outValid & (1 << i)) && data == blockedBits[i],
                "valid output token changed while backpressured");
      blocked[i] = !reset && (outValid & (1 << i)) && !evalField(4 + i, "ready");
      blockedBits[i] = data;
      if (pending[i] && (inReady & (1 << i))) { pending[i] = false; ++inputCount[i]; }
      if (evalField(4 + i, "ready") && (outValid & (1 << i)) && !reset) ++outputCount[i];
      edges[i] += (ceMask >> i) & 1;
      if ((ceMask >> i) & 1) expectedState[i] = (expectedState[i] + inputBits[i]) & 65535;
    }
    sim.edge();
    // Check the connected producer holds its next edge token while blocked.
    if (!done && !reset) {
      sim.memo.clear();
      require((sim.eval(tokens[0]) | sim.eval(tokens[1]) << 1) == heldMask,
              "producer token changed under hub backpressure");
    }
  }
  require(stalls > 2000 && unequal > 1000 && completed > 500 &&
          edges[0] > 200 && edges[1] > 200 && inputCount[0] > 200 && inputCount[1] > 200 &&
          outputCount[0] > 200 && outputCount[1] > 200, "coupled schedule lacks stall/domain coverage");
  llvm::errs() << "Coupled hub: " << completed << " completions, " << stalls << " stalled host cycles, "
               << edges[0] << '/' << edges[1] << " target edges\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc <= 3 && (argc == 1 || StringRef(argv[1]) == "--normal" ||
                         StringRef(argv[1]) == "--reversed"),
            "usage: FAMEHubClockCoupledTest [--normal|--reversed [output.mlir]]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    clockRecordRejections(ctx);
    outputValidRejections(ctx);
    bool reversed = argc > 1 && StringRef(argv[1]) == "--reversed";
    run(ctx, reversed, argc > 2 ? argv[2] : nullptr);
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
