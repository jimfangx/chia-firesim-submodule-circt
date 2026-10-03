// See LICENSE for license details.
#include "goldengate/FAMEFiredState.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/FAMEFinishing.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, bool clockToken = false) {
  std::string text = R"mlir(
    module { firrtl.circuit "Model" {
      firrtl.module @Model(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %enable: !firrtl.uint<1>,
        in %done: !firrtl.uint<1>,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        in %virtualInput_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %virtualOutput_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
        %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
        firrtl.strictconnect %targetCycleFinishing, %done : !firrtl.uint<1>
      }
    } }
  )mlir";
  if (clockToken) {
    auto pos = text.find(") {");
    text.insert(pos,
                ", in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: "
                "uint<1>, bits: clock>");
  }
  return parseSourceString<ModuleOp>(text, &context);
}
FModuleOp model(ModuleOp root) {
  auto circuit = *root.getOps<CircuitOp>().begin();
  return *circuit.getOps<FModuleOp>().begin();
}
SmallVector<goldengate::FAMEFiredChannel> channels(FModuleOp m) {
  OpBuilder b(&m.getBodyBlock()->front());
  auto one = b.create<ConstantOp>(
      m.getLoc(), UIntType::get(m.getContext(), 1), APInt(1, 1));
  return {{"input", true, m.getBodyBlock()->getArgument(2)},
          {"output", false, m.getBodyBlock()->getArgument(2)},
          {"virtualInput", true, one, false},
          {"virtualOutput", false, one, false}};
}
unsigned eval(Value v, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto it = values.find(v); it != values.end())
    return it->second;
  if (auto c = v.getDefiningOp<ConstantOp>())
    return c.getValue().getZExtValue();
  if (auto w = v.getDefiningOp<WireOp>()) {
    for (auto *user : w.getResult().getUsers())
      if (auto c = dyn_cast<StrictConnectOp>(user); c && c.getDest() == v)
        return eval(c.getSrc(), values);
  }
  if (auto p = v.getDefiningOp<NotPrimOp>())
    return !eval(p.getInput(), values);
  if (auto p = v.getDefiningOp<AndPrimOp>())
    return eval(p.getLhs(), values) & eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<OrPrimOp>())
    return eval(p.getLhs(), values) | eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<MuxPrimOp>())
    return eval(p.getSel(), values) ? eval(p.getHigh(), values)
                                    : eval(p.getLow(), values);
  if (auto p = v.getDefiningOp<AsUIntPrimOp>())
    return eval(p.getInput(), values);
  throw std::runtime_error("unexpected transition operation");
}
void behavior(MLIRContext &context) {
  auto root = fixture(context);
  require(bool(root), "fixture parse failed");
  auto m = model(*root);
  auto cs = channels(m);
  std::string error;
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  std::string created = dump(*root);
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  require(created == dump(*root), "register creation is not idempotent");
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)),
          error.c_str());
  require(succeeded(verify(*root)), "rewritten IR does not verify");
  unsigned states = 0;
  for (const auto &ch : cs) {
    RegResetOp reg;
    for (auto r : m.getOps<RegResetOp>())
      if (r.getName() == ch.name + "_fired_0")
        reg = r;
    unsigned reset = unsigned(ch.isInput && ch.hasClockDomain);
    require(reg && reg.getClockVal() == m.getBodyBlock()->getArgument(0) &&
                reg.getResetSignal() == m.getBodyBlock()->getArgument(1),
            "wrong host controls");
    require(eval(reg.getResetValue(), llvm::DenseMap<Value, unsigned>()) ==
                reset,
            "wrong reset value");
    StrictConnectOp transition;
    for (auto c : m.getOps<StrictConnectOp>())
      if (c.getDest() == reg.getResult())
        transition = c;
    require(bool(transition), "missing fired transition");
    // SFC genMetadata/updateFiredReg: reset has priority over completion,
    // completion has priority over a coincident ready/valid handshake.
    for (unsigned flags = 0; flags < 64; ++flags) {
      unsigned old = flags & 1, ready = (flags >> 1) & 1,
               valid = (flags >> 2) & 1;
      unsigned done = (flags >> 3) & 1, enable = (flags >> 4) & 1,
               hostReset = (flags >> 5) & 1;
      llvm::DenseMap<Value, unsigned> values{
          {reg.getResult(), old},
          {m.getBodyBlock()->getArgument(2), enable},
          {m.getBodyBlock()->getArgument(3), done}};
      for (auto f : m.getOps<SubfieldOp>())
        if (f.getInput() ==
            m.getBodyBlock()->getArgument(ch.name == "input"          ? 4
                                          : ch.name == "output"       ? 5
                                          : ch.name == "virtualInput" ? 6
                                                                      : 7))
          values[f.getResult()] = f.getFieldName() == "ready" ? ready : valid;
      unsigned result = hostReset ? eval(reg.getResetValue(), values)
                                  : eval(transition.getSrc(), values);
      unsigned expected = hostReset ? reset
                          : done    ? !(ch.hasClockDomain ? enable : 1)
                                    : old | (ready & valid);
      require(result == expected,
              "reset/finish/handshake priority differs from SFC");
      ++states;
    }
  }
  llvm::outs() << "Passed " << states
               << " clocked/virtual input/output transition and reset cases\n";
}
void rejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 7; ++bad) {
    auto root = fixture(context);
    require(bool(root), "fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    RegResetOp last;
    for (auto reg : m.getOps<RegResetOp>())
      if (reg.getName() == "virtualOutput_fired_0")
        last = reg;
    OpBuilder b(last);
    if (bad == 0 || bad == 1) {
      RegResetOp reg = last;
      if (bad == 1)
        for (auto r : m.getOps<RegResetOp>())
          if (r.getName() == "input_fired_0")
            reg = r;
      reg.getResetValueMutable().assign(b.create<ConstantOp>(
          m.getLoc(), UIntType::get(&context, 1), APInt(1, bad == 0)));
    }
    if (bad == 2)
      last.getResetValueMutable().assign(m.getBodyBlock()->getArgument(2));
    if (bad == 3)
      last.getClockValMutable().assign(b.create<AsClockPrimOp>(
          m.getLoc(), m.getBodyBlock()->getArgument(1)));
    if (bad == 4)
      last.getResetSignalMutable().assign(m.getBodyBlock()->getArgument(2));
    if (bad == 5)
      cs.back().clockDomainEnable = m.getBodyBlock()->getArgument(2);
    if (bad == 6)
      cs.back().clockDomainEnable = b.create<ConstantOp>(
          m.getLoc(), UIntType::get(&context, 1), APInt(1, 0));
    auto before = dump(*root);
    require(failed(goldengate::rewriteFAMEFiredStates(m, cs, error)) &&
                !error.empty(),
            "invalid contract accepted");
    require(before == dump(*root), "failed rewrite partially mutated channels");
    if (bad < 5) {
      require(failed(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
              "invalid existing reset accepted");
      require(before == dump(*root), "failed register creation mutated IR");
    }
  }
  llvm::outs()
      << "Passed seven atomic reset/host-control/virtual-enable rejections\n";
}
// FAMETransform topRules: VirtualClockChannel.isValid = 1 and
// VirtualClockChannel.setReady = EmptyStmt. Exercise the same data predicate
// with both a real clock token and the virtual channel.
void finishingBehavior(MLIRContext &context) {
  unsigned cases = 0;
  for (bool clockToken : {false, true}) {
    auto root = fixture(context, clockToken);
    require(bool(root), "finishing fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    unsigned ports = m.getNumPorts();
    require(
        succeeded(goldengate::rewriteFAMEFinishing(m,
                                                   {"input", "virtualInput"},
                                                   {"output", "virtualOutput"},
                                                   clockToken ? "tick" : "",
                                                   error)),
        error.c_str());
    require(succeeded(verify(*root)), "finishing IR failed verification");
    require(m.getNumPorts() == ports, "virtual clock created a model port");
    Value next, clockReady;
    for (auto c : m.getOps<StrictConnectOp>()) {
      if (auto w = c.getDest().getDefiningOp<WireOp>();
          w && w.getName() == "targetCycleFinishing")
        next = c.getSrc();
      if (auto f = c.getDest().getDefiningOp<SubfieldOp>();
          clockToken && f && f.getInput() == m.getBodyBlock()->getArgument(8) &&
          f.getFieldName() == "ready")
        clockReady = c.getSrc();
    }
    require(bool(next) && bool(clockReady) == clockToken,
            "wrong finishing/clock ready controls");
    for (unsigned flags = 0; flags < (clockToken ? 512u : 256u); ++flags) {
      unsigned inputValid = flags & 1, virtualValid = (flags >> 1) & 1;
      unsigned outFired = (flags >> 2) & 1, outReady = (flags >> 3) & 1,
               outValid = (flags >> 4) & 1;
      unsigned virtualFired = (flags >> 5) & 1, virtualReady = (flags >> 6) & 1,
               virtualOutValid = (flags >> 7) & 1;
      unsigned tickValid = clockToken ? (flags >> 8) & 1 : 1;
      llvm::DenseMap<Value, unsigned> values;
      for (auto r : m.getOps<RegResetOp>()) {
        if (r.getName() == "output_fired_0")
          values[r.getResult()] = outFired;
        if (r.getName() == "virtualOutput_fired_0")
          values[r.getResult()] = virtualFired;
      }
      for (auto f : m.getOps<SubfieldOp>()) {
        auto port = dyn_cast<BlockArgument>(f.getInput());
        if (!port)
          continue;
        unsigned i = port.getArgNumber();
        if (i == 4 && f.getFieldName() == "valid")
          values[f.getResult()] = inputValid;
        if (i == 6 && f.getFieldName() == "valid")
          values[f.getResult()] = virtualValid;
        if (i == 5)
          values[f.getResult()] =
              f.getFieldName() == "ready" ? outReady : outValid;
        if (i == 7)
          values[f.getResult()] =
              f.getFieldName() == "ready" ? virtualReady : virtualOutValid;
        if (i == 8 && f.getFieldName() == "valid")
          values[f.getResult()] = tickValid;
      }
      unsigned dataReady = inputValid & virtualValid &
                           (outFired | (outReady & outValid)) &
                           (virtualFired | (virtualReady & virtualOutValid));
      require(eval(next, values) == (dataReady & tickValid),
              "finishing differs from SFC topRules");
      if (clockToken)
        require(eval(clockReady, values) == dataReady,
                "clock ready must not depend on clock valid");
      ++cases;
    }
  }
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(context, bad == 3 || bad == 5);
    require(bool(root), "rejection fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    if (bad == 5)
      cs.push_back({"tick", true, m.getBodyBlock()->getArgument(2)});
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    SmallVector<std::string> inputs{"input", "virtualInput"},
        outputs{"output", "virtualOutput"};
    if (bad == 0)
      inputs.pop_back();
    if (bad == 1)
      inputs.push_back("input");
    if (bad == 4)
      outputs.pop_back();
    if (bad == 5)
      inputs.push_back("tick");
    auto before = dump(*root);
    require(failed(goldengate::rewriteFAMEFinishing(
                m, inputs, outputs, bad == 2 ? "missing" : "", error)) &&
                !error.empty(),
            "invalid finishing channel coverage accepted");
    require(before == dump(*root), "failed finishing rewrite mutated IR");
  }
  llvm::outs() << "Passed " << cases
               << " clocked/virtual completion cases and six atomic coverage "
                  "rejections\n";
}
// Emit named boolean operations for a structured comparison with optimized
// SFC RTL. This preserves operands and channel identity while ignoring SSA IDs.
std::string predicate(FModuleOp m, Value value) {
  if (auto f = value.getDefiningOp<SubfieldOp>()) {
    if (auto port = dyn_cast<BlockArgument>(f.getInput()))
      return m.getPortName(port.getArgNumber()).str() + "_" +
             f.getFieldName().str();
  }
  if (auto r = value.getDefiningOp<RegResetOp>())
    return r.getName().str();
  if (auto c = value.getDefiningOp<ConstantOp>())
    return std::to_string(c.getValue().getZExtValue());
  if (auto a = value.getDefiningOp<AndPrimOp>())
    return "(" + predicate(m, a.getLhs()) + " & " + predicate(m, a.getRhs()) +
           ")";
  if (auto o = value.getDefiningOp<OrPrimOp>())
    return "(" + predicate(m, o.getLhs()) + " | " + predicate(m, o.getRhs()) +
           ")";
  throw std::runtime_error("unexpected completion operation");
}
void virtualControls(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "virtual control boundary parse failed");
  auto m = model(*root);
  require(m.getNumPorts() == 7, "virtual clock gained a token port");
  std::map<std::string, Value> registers, ports;
  for (auto r : m.getOps<RegResetOp>()) {
    registers[r.getName().str()] = r.getResult();
    if (r.getName().ends_with("_fired_0"))
      require(eval(r.getResetValue(), {}) ==
                  unsigned(r.getName() == "input_fired_0"),
              "clock association did not select the SFC reset value");
  }
  require(registers.size() == 5, "missing channel state or clock buffer");
  for (unsigned i = 0; i < m.getNumPorts(); ++i)
    ports[m.getPortName(i).str()] = m.getBodyBlock()->getArgument(i);
  auto drive = [&](Value dest) -> Value {
    auto sameDestination = [&](Value candidate) {
      if (dest == candidate)
        return true;
      auto a = dest.getDefiningOp<SubfieldOp>();
      auto b = candidate.getDefiningOp<SubfieldOp>();
      return a && b && a.getInput() == b.getInput() &&
             a.getFieldIndex() == b.getFieldIndex();
    };
    for (auto c : m.getOps<ConnectOp>())
      if (sameDestination(c.getDest()))
        return c.getSrc();
    for (auto c : m.getOps<StrictConnectOp>())
      if (sameDestination(c.getDest()))
        return c.getSrc();
    throw std::runtime_error("control destination is undriven");
  };
  Value finishing;
  for (auto w : m.getOps<WireOp>())
    if (w.getName() == "targetCycleFinishing")
      finishing = w.getResult();
  require(bool(finishing), "missing virtual cycle completion");
  InstanceOp gate;
  for (auto instance : m.getOps<InstanceOp>())
    if (instance.getName() == "target_buffer")
      gate = instance;
  require(gate && gate.getModuleName() == "AbstractClockGate" &&
              gate.getNumResults() == 3 &&
              !gate->hasAttr("goldengate.generatedClockConstraint") &&
              drive(gate.getResult(0)) == ports.at("hostClock"),
          "virtual target gate has the wrong interface or input clock");
  require(ports.at("target").use_empty(), "ungated target-clock uses remain");
  auto state = *m.getOps<RegOp>().begin();
  require(state.getClockVal() == gate.getResult(2),
          "target state did not move to the gated host clock");
  RegResetOp enabled;
  for (auto r : m.getOps<RegResetOp>())
    if (r.getName() == "target_enabled")
      enabled = r;
  require(enabled.getClockVal() == ports.at("hostClock") &&
              enabled.getResetSignal() == ports.at("hostReset") &&
              eval(enabled.getResetValue(), llvm::DenseMap<Value, unsigned>()) == 0,
          "virtual enable reset/clock differs from SFC");
  for (unsigned flags = 0; flags < 8; ++flags) {
    unsigned old = flags & 1, done = (flags >> 1) & 1,
             reset = (flags >> 2) & 1;
    llvm::DenseMap<Value, unsigned> values{
        {enabled.getResult(), old}, {finishing, done},
        {ports.at("hostReset"), reset}};
    require(eval(drive(gate.getResult(1)), values) == (old & done & !reset),
            "virtual gate advances during reset/stall or before initialization");
    require((reset ? eval(enabled.getResetValue(), values)
                   : eval(drive(enabled.getResult()), values)) ==
                (reset ? 0 : done ? 1 : old),
            "virtual enable completion/hold differs from SFC");
  }
  llvm::outs() << "Matched 8 virtual-clock buffer/gate reset/hold/advance cases\n";
  for (unsigned flags = 0; flags < 256; ++flags) {
    unsigned inValid = flags & 1, virtualValid = (flags >> 1) & 1;
    unsigned fired = (flags >> 2) & 1, virtualFired = (flags >> 3) & 1;
    unsigned ready = (flags >> 4) & 1, virtualReady = (flags >> 5) & 1;
    unsigned inFired = (flags >> 6) & 1, virtualInFired = (flags >> 7) & 1;
    unsigned outValid = inValid & !fired,
             virtualOutValid = virtualValid & !virtualFired;
    unsigned complete = inValid & virtualValid & (fired | ready) &
                        (virtualFired | virtualReady);
    llvm::DenseMap<Value, unsigned> values{
        {finishing, complete},
        {registers.at("input_fired_0"), inFired},
        {registers.at("virtualInput_fired_0"), virtualInFired},
        {registers.at("output_fired_0"), fired},
        {registers.at("virtualOutput_fired_0"), virtualFired}};
    for (auto f : m.getOps<SubfieldOp>()) {
      if (f.getFieldName() == "valid") {
        if (f.getInput() == ports.at("input_sink"))
          values[f.getResult()] = inValid;
        if (f.getInput() == ports.at("virtualInput_sink"))
          values[f.getResult()] = virtualValid;
        if (f.getInput() == ports.at("output_source"))
          values[f.getResult()] = outValid;
        if (f.getInput() == ports.at("virtualOutput_source"))
          values[f.getResult()] = virtualOutValid;
      }
      if (f.getFieldName() == "ready") {
        if (f.getInput() == ports.at("output_source"))
          values[f.getResult()] = ready;
        if (f.getInput() == ports.at("virtualOutput_source"))
          values[f.getResult()] = virtualReady;
        if (f.getInput() == ports.at("input_sink"))
          values[f.getResult()] = complete & !inFired;
        if (f.getInput() == ports.at("virtualInput_sink"))
          values[f.getResult()] = complete & !virtualInFired;
      }
    }
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
      values[registers.at("target_enabled")] = enabled;
      auto transition = [&](const char *name,
                            unsigned old,
                            unsigned handshake,
                            unsigned enable) {
        require(eval(drive(registers.at(name)), values) ==
                    (complete ? !enable : old | handshake),
                "compiler-emitted fired transition has wrong clock enable");
      };
      transition("input_fired_0", inFired, inValid & (complete & !inFired), 1);
      transition("virtualInput_fired_0",
                 virtualInFired,
                 virtualValid & (complete & !virtualInFired),
                 1);
      transition("output_fired_0", fired, ready & outValid, enabled);
      transition("virtualOutput_fired_0",
                 virtualFired,
                 virtualReady & virtualOutValid,
                 1);
    }
    require(eval(drive(finishing), values) == complete,
            "virtual finishing differs from SFC topRules");
    for (auto f : m.getOps<SubfieldOp>()) {
      Value port = f.getInput();
      if (f.getFieldName() == "valid" && port == ports.at("output_source"))
        require(eval(drive(f.getResult()), values) == outValid,
                "clocked output valid differs");
      if (f.getFieldName() == "valid" &&
          port == ports.at("virtualOutput_source"))
        require(eval(drive(f.getResult()), values) == virtualOutValid,
                "clockless output valid differs");
      if (f.getFieldName() == "ready" && port == ports.at("input_sink"))
        require(eval(drive(f.getResult()), values) == (complete & !inFired),
                "clocked input ready differs");
      if (f.getFieldName() == "ready" && port == ports.at("virtualInput_sink"))
        require(eval(drive(f.getResult()), values) ==
                    (complete & !virtualInFired),
                "clockless input ready differs");
    }
  }
  llvm::outs() << "Passed 256 compiler-emitted virtual channel-control cases "
                  "and 2048 fired transitions\n";
}
// Emit the actual CIRCT buffer/gate truth table for comparison with the SFC
// RTL boundary. Token substitution also tests the virtual constant-one case.
void clockControls(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "clock control boundary parse failed");
  unsigned gates = 0;
  root->walk([&](FModuleOp m) {
    for (auto gate : m.getOps<InstanceOp>()) {
      if (gate.getModuleName() != "AbstractClockGate")
        continue;
      require(gate.getName().ends_with("_buffer"), "unexpected clock gate name");
      auto stem = gate.getName().drop_back(7); // _buffer
      RegResetOp enabled;
      Value finishing, hostClock, hostReset;
      for (unsigned i = 0; i < m.getNumPorts(); ++i) {
        if (m.getPortName(i) == "hostClock")
          hostClock = m.getBodyBlock()->getArgument(i);
        if (m.getPortName(i) == "hostReset")
          hostReset = m.getBodyBlock()->getArgument(i);
      }
      for (auto r : m.getOps<RegResetOp>())
        if (r.getName() == (stem + "_enabled").str())
          enabled = r;
      for (auto w : m.getOps<WireOp>())
        if (w.getName() == "targetCycleFinishing")
          finishing = w.getResult();
      auto drive = [&](Value dest) -> Value {
        for (auto c : m.getOps<StrictConnectOp>())
          if (c.getDest() == dest)
            return c.getSrc();
        for (auto c : m.getOps<ConnectOp>())
          if (c.getDest() == dest)
            return c.getSrc();
        throw std::runtime_error("clock control has no driver");
      };
      require(enabled && finishing && hostClock && hostReset &&
                  drive(gate.getResult(0)) == hostClock &&
                  enabled.getClockVal() == hostClock &&
                  enabled.getResetSignal() == hostReset,
              "clock buffer/gate host controls differ");
      Value next = drive(enabled.getResult());
      auto mux = next.getDefiningOp<MuxPrimOp>();
      require(mux && mux.getSel() == finishing &&
                  mux.getLow() == enabled.getResult(),
              "clock buffer does not hold until completion");
      bool virtualToken = bool(mux.getHigh().getDefiningOp<ConstantOp>());
      for (unsigned flags = 0; flags < 16; ++flags) {
        unsigned old = flags & 1, done = (flags >> 1) & 1,
                 reset = (flags >> 2) & 1, token = (flags >> 3) & 1;
        llvm::DenseMap<Value, unsigned> values{
            {enabled.getResult(), old}, {finishing, done}, {hostReset, reset}};
        if (!virtualToken)
          values[mux.getHigh()] = token;
        unsigned actualNext = reset ? eval(enabled.getResetValue(), values)
                                    : eval(next, values);
        unsigned ce = eval(drive(gate.getResult(1)), values);
        require(actualNext == (reset ? 0 : done ? (virtualToken ? 1 : token) : old)
                    && ce == (old & done & !reset),
                "clock buffer/gate differs from SFC");
        llvm::outs() << "CLOCK_CASE " << m.getName() << " " << flags << " "
                     << actualNext << " " << ce << "\n";
      }
      ++gates;
    }
  });
  require(gates != 0, "no clock gate found at boundary");
  require(succeeded(verify(*root)), "clock control boundary does not verify");
}
void boundary(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "candidate boundary parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp m;
  for (auto candidate : circuit.getOps<FModuleOp>())
    if (candidate.getName() == "FireSim")
      m = candidate;
  require(bool(m), "FireSim model missing at boundary");
  Value clockPort, outputEnable;
  for (unsigned i = 0; i < m.getNumPorts(); ++i)
    if (m.getPortName(i) == "clockBridge_clocks_0_sink")
      clockPort = m.getBodyBlock()->getArgument(i);
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName() == "clockBridge_clocks_0_enabled")
      outputEnable = reg.getResult();
  require(clockPort && outputEnable, "missing target clock controls");
  OpBuilder b(&m.getBodyBlock()->front());
  Value inputEnable = b.create<AsUIntPrimOp>(
      m.getLoc(), b.create<SubfieldOp>(m.getLoc(), clockPort, "bits"));
  SmallVector<goldengate::FAMEFiredChannel> cs;
  for (unsigned i = 0; i < m.getNumPorts(); ++i) {
    auto name = m.getPortName(i);
    if (name == "clockBridge_clocks_0_sink")
      continue;
    if (name.ends_with("_sink"))
      cs.push_back({name.drop_back(5).str(), true, inputEnable});
    if (name.ends_with("_source"))
      cs.push_back({name.drop_back(7).str(), false, outputEnable});
  }
  require(cs.size() == 42, "unexpected Rocket data channel count");
  auto before = dump(m);
  std::string error;
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  require(before == dump(m), "oracle boundary register declaration changed");
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)),
          error.c_str());
  SmallVector<std::string> inputs, outputs;
  for (const auto &ch : cs)
    (ch.isInput ? inputs : outputs).push_back(ch.name);
  require(succeeded(goldengate::rewriteFAMEFinishing(
              m, inputs, outputs, "clockBridge_clocks_0", error)),
          error.c_str());
  auto emitPredicate = [&](Value dest, Value src) {
    if (auto w = dest.getDefiningOp<WireOp>();
        w && w.getName() == "targetCycleFinishing")
      llvm::outs() << "COMPLETION " << predicate(m, src) << "\n";
    if (auto f = dest.getDefiningOp<SubfieldOp>();
        f && f.getInput() == clockPort && f.getFieldName() == "ready")
      llvm::outs() << "CLOCK_READY " << predicate(m, src) << "\n";
  };
  for (auto c : m.getOps<ConnectOp>())
    emitPredicate(c.getDest(), c.getSrc());
  for (auto c : m.getOps<StrictConnectOp>())
    emitPredicate(c.getDest(), c.getSrc());
  require(succeeded(verify(m)), "candidate model failed verification");
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName().ends_with("_fired_0")) {
      require(reg.getClockVal() == m.getBodyBlock()->getArgument(0) &&
                  reg.getResetSignal() == m.getBodyBlock()->getArgument(1),
              "boundary host controls differ");
      llvm::outs() << "BOUNDARY " << reg.getName() << " "
                   << eval(reg.getResetValue(),
                           llvm::DenseMap<Value, unsigned>())
                   << "\n";
    }
  unsigned cases = 0;
  for (const auto &ch : cs) {
    RegResetOp reg;
    Value port;
    for (auto r : m.getOps<RegResetOp>())
      if (r.getName() == ch.name + "_fired_0")
        reg = r;
    for (unsigned i = 0; i < m.getNumPorts(); ++i)
      if (m.getPortName(i) == ch.name + (ch.isInput ? "_sink" : "_source"))
        port = m.getBodyBlock()->getArgument(i);
    Value next;
    for (auto c : m.getOps<ConnectOp>())
      if (c.getDest() == reg.getResult())
        next = c.getSrc();
    for (auto c : m.getOps<StrictConnectOp>())
      if (c.getDest() == reg.getResult())
        next = c.getSrc();
    Value finishing;
    for (auto w : m.getOps<WireOp>())
      if (w.getName() == "targetCycleFinishing")
        finishing = w.getResult();
    require(reg && port && next && finishing, "incomplete boundary transition");
    for (unsigned flags = 0; flags < 64; ++flags) {
      unsigned old = flags & 1, ready = (flags >> 1) & 1,
               valid = (flags >> 2) & 1;
      unsigned done = (flags >> 3) & 1, enable = (flags >> 4) & 1,
               hostReset = (flags >> 5) & 1;
      llvm::DenseMap<Value, unsigned> values{{reg.getResult(), old},
                                             {finishing, done},
                                             {ch.clockDomainEnable, enable}};
      for (auto f : m.getOps<SubfieldOp>())
        if (f.getInput() == port) {
          if (f.getFieldName() == "ready")
            values[f.getResult()] = ready;
          if (f.getFieldName() == "valid")
            values[f.getResult()] = valid;
        }
      unsigned actual =
          hostReset ? eval(reg.getResetValue(), values) : eval(next, values);
      require(actual == (hostReset ? unsigned(ch.isInput)
                         : done    ? !enable
                                   : old | (ready & valid)),
              "Rocket transition/reset priority differs from SFC");
      ++cases;
    }
  }
  llvm::outs() << "Passed " << cases
               << " Rocket reset/completion/handshake combinations\n";
  llvm::outs() << "Validated all 42 Rocket fired-register reset contracts and "
                  "rewrote transitions\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context);
    rejections(context);
    finishingBehavior(context);
    if (argc == 3 && std::string(argv[1]) == "--virtual-controls")
      virtualControls(context, argv[2]);
    else if (argc == 3 && std::string(argv[1]) == "--clock-controls")
      clockControls(context, argv[2]);
    else if (argc == 2)
      boundary(context, argv[1]);
    else
      require(argc == 1,
              "usage: FAMEFiredStateTest [candidate.mlir | --virtual-controls "
              "candidate.mlir | --clock-controls candidate.mlir]");
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
