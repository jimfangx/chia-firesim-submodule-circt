// See LICENSE for license details.
// The companion Scala oracle executes the preserved SFC FAMETransform.
#include "goldengate/FAMEModelControls.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEFiredRegister.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
using namespace goldengate;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream o(s); op->print(o); return s; }
std::string key(Value value) {
  if (auto field = value.getDefiningOp<SubfieldOp>()) return key(field.getInput()) + "." + field.getFieldName().str();
  return std::to_string(reinterpret_cast<uintptr_t>(value.getAsOpaquePointer()));
}
FModuleOp lookup(CircuitOp circuit, StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing model");
}
Value port(FModuleOp model, StringRef name) {
  for (unsigned i = 0; i < model.getNumPorts(); ++i) if (model.getPortName(i) == name) return model.getArgument(i);
  throw std::runtime_error("missing model port");
}
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  FModuleOp hub, worker;
  SmallVector<FAMEModelControlPlan, 0> plans;
  Fixture(MLIRContext &ctx, bool associated) {
    root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
      firrtl.module @Top(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %target0: !firrtl.clock, in %target1: !firrtl.clock) {}
      firrtl.module private @Hub(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<c0: clock, c1: clock>>,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        in %right_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %other_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
        %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      }
      firrtl.module private @Worker(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
        %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      }
    } })mlir", &ctx);
    require(bool(root), "fixture parse"); circuit = *root->getOps<CircuitOp>().begin();
    hub = lookup(circuit, "Hub"); worker = lookup(circuit, "Worker");
    OpBuilder annotations(&ctx);
    circuit->setAttr("rawAnnotations", annotations.getArrayAttr({annotations.getDictionaryAttr({
      annotations.getNamedAttr("class", annotations.getStringAttr("test.Preserved")),
      annotations.getNamedAttr("target", annotations.getStringAttr("~Top|Hub>state0"))})}));
    auto addState = [&](FModuleOp model, StringRef name, Value clock, StringRef in, StringRef out) {
      OpBuilder b(model.getBodyBlock(), model.getBodyBlock()->end()); auto loc = model.getLoc();
      Value data = b.create<SubfieldOp>(loc, port(model, in), "bits");
      Value reset = b.create<BitsPrimOp>(loc, data, 7, 7);
      Value zero = b.create<ConstantOp>(loc, UIntType::get(&ctx, 8), APInt(8, 0));
      auto state = b.create<RegResetOp>(loc, UIntType::get(&ctx, 8), clock, reset, zero, name);
      Value increment = b.create<BitsPrimOp>(loc, data, 6, 0);
      Value sum = b.create<AddPrimOp>(loc, state.getResult(), increment);
      Value result = b.create<BitsPrimOp>(loc, sum, 7, 0);
      b.create<StrictConnectOp>(loc, state.getResult(), result);
      Value bits = b.create<SubfieldOp>(loc, port(model, out), "bits");
      b.create<StrictConnectOp>(loc, bits, result);
    };
    OpBuilder hb(hub.getBodyBlock(), hub.getBodyBlock()->end());
    Value tick = hb.create<SubfieldOp>(hub.getLoc(), port(hub, "tick_sink"), "bits");
    Value c0 = hb.create<SubfieldOp>(hub.getLoc(), tick, "c0"), c1 = hb.create<SubfieldOp>(hub.getLoc(), tick, "c1");
    addState(hub, "state0", c0, "input_sink", "output_source");
    addState(hub, "state1", c1, "right_sink", "other_source");
    addState(worker, "state", port(worker, "target"), "input_sink", "output_source");
    auto top = lookup(circuit, "Top"); OpBuilder tb(top.getBodyBlock(), top.getBodyBlock()->end());
    auto hi = tb.create<InstanceOp>(top.getLoc(), hub, "hub");
    hi->setAttr("test.preserved", tb.getStringAttr("hub metadata"));
    for (unsigned i = 0; i < 2; ++i) tb.create<StrictConnectOp>(top.getLoc(), hi.getResult(i), top.getArgument(i));
    for (unsigned index = 0; index < 2; ++index) {
      auto wi = tb.create<InstanceOp>(top.getLoc(), worker, "worker" + std::to_string(index));
      wi->setAttr("test.preserved", tb.getStringAttr("worker metadata"));
      for (unsigned i = 0; i < 2; ++i) tb.create<StrictConnectOp>(top.getLoc(), wi.getResult(i), top.getArgument(i));
      tb.create<StrictConnectOp>(top.getLoc(), wi.getResult(2), top.getArgument(2 + index));
    }
    SmallVector<FAMEHubClockDomain> domains{
      {0, 0, "c0", "top0", "c0", {"d0", 1, 2, 2}},
      {0, 0, "c1", "top1", "c1", {"d1", 1, 3, 3}}};
    std::string error;
    auto clocks = constructFAMEHubClockControls(circuit, hub, "tick", domains, error); require(bool(clocks), error);
    auto virtualClock = constructFAMEVirtualClockControls(circuit, worker, "target", error); require(bool(virtualClock), error);
    FAMEModelControlPlan hubPlan; hubPlan.model = hub; hubPlan.clockChannel = "tick";
    hubPlan.clocks.assign(clocks->begin(), clocks->end());
    hubPlan.channels = {{"input", Direction::In, "c0"}, {"right", Direction::In, "c1"},
      {"output", Direction::Out, "c0"}, {"other", Direction::Out, "c1"}};
    LocalChannelDependency left, right; left.outputChannel = "output"; left.inputChannels = {"input"};
    right.outputChannel = "other"; right.inputChannels = {"right"}; hubPlan.dependencies = {left, right};
    FAMEModelControlPlan workerPlan; workerPlan.model = worker; workerPlan.clocks = {*virtualClock};
    std::optional<std::string> association = associated ? std::optional<std::string>("target") : std::nullopt;
    workerPlan.channels = {{"input", Direction::In, association}, {"output", Direction::Out, association}};
    workerPlan.dependencies = {left}; plans = {std::move(hubPlan), std::move(workerPlan)};
  }
};
struct Interpreter {
  FModuleOp model;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> state, inputs;
  FAMEClockGateIndex gates;
  FAMEFiredRegisterIndex fired;
  FAMEClockEnableIndex enabled;
  explicit Interpreter(FModuleOp m) : model(m) {
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    for (auto r : m.getOps<RegResetOp>()) state[r.getName().str()] = r.getName().starts_with("state") ? 37 : 0;
    std::string error; require(succeeded(gates.collect(m, error)) && succeeded(fired.collect(m, error)) && succeeded(enabled.collect(m, error)), error);
  }
  uint64_t eval(Value v) {
    if (auto it = inputs.find(key(v)); it != inputs.end()) return it->second;
    if (auto r = v.getDefiningOp<RegResetOp>()) return state.at(r.getName().str());
    if (auto c = v.getDefiningOp<ConstantOp>()) return c.getValue().getZExtValue();
    if (auto it = drivers.find(key(v)); it != drivers.end()) return eval(it->second);
    uint64_t value;
    if (auto p = v.getDefiningOp<NotPrimOp>()) value = ~eval(p.getInput());
    else if (auto p = v.getDefiningOp<AndPrimOp>()) value = eval(p.getLhs()) & eval(p.getRhs());
    else if (auto p = v.getDefiningOp<OrPrimOp>()) value = eval(p.getLhs()) | eval(p.getRhs());
    else if (auto p = v.getDefiningOp<AddPrimOp>()) value = eval(p.getLhs()) + eval(p.getRhs());
    else if (auto p = v.getDefiningOp<BitsPrimOp>()) value = eval(p.getInput()) >> p.getLo();
    else if (auto p = v.getDefiningOp<MuxPrimOp>()) value = eval(eval(p.getSel()) ? p.getHigh() : p.getLow());
    else if (auto p = v.getDefiningOp<AsUIntPrimOp>()) value = eval(p.getInput());
    else throw std::runtime_error("unsupported emitted equation");
    auto type = dyn_cast<UIntType>(v.getType()); auto width = type ? type.getWidth() : std::nullopt;
    require(width && *width > 0 && *width < 64, "integer width");
    return value & ((uint64_t(1) << *width) - 1);
  }
  uint64_t field(StringRef p, StringRef name) {
    auto k = key(port(model, p)) + "." + name.str();
    if (auto it = drivers.find(k); it != drivers.end()) return eval(it->second);
    return inputs.at(k);
  }
  uint64_t finishing() {
    for (auto w : model.getOps<WireOp>()) if (w.getName() == "targetCycleFinishing") return eval(w.getResult());
    throw std::runtime_error("missing finishing");
  }
  uint64_t gate(StringRef clock) { return eval(gates.lookup(clock).getResult(1)); }
  void resetHost() {
    for (auto r : model.getOps<RegResetOp>()) if (r.getClockVal() == port(model, "hostClock")) state[r.getName().str()] = eval(r.getResetValue());
  }
  void step() {
    auto next = state;
    for (auto r : model.getOps<RegResetOp>()) {
      bool advance = r.getClockVal() == port(model, "hostClock");
      for (auto instance : model.getOps<InstanceOp>()) if (instance.getModuleName() == "AbstractClockGate" && r.getClockVal() == instance.getResult(2)) advance = eval(instance.getResult(1));
      if (advance) next[r.getName().str()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    }
    state = std::move(next);
  }
};
unsigned payload(unsigned n, unsigned stream) { return n == 0 || n % 11 == 7 ? 128 : (n * 17 + stream * 9) % 127; }
std::string simulate(Fixture &f, bool associated) {
  std::string trace; llvm::raw_string_ostream out(trace);
  Interpreter hub(f.hub), workers[]{Interpreter(f.worker), Interpreter(f.worker)};
  unsigned tokens[4]{}, clockIndex = 0;
  for (unsigned cycle = 0; cycle < 96; ++cycle) {
    bool reset = cycle < 2 || cycle == 40 || cycle == 41;
    if (reset) clockIndex = 0;
    bool clockValid = cycle % 9 != 5;
    hub.inputs = {{key(port(f.hub, "hostReset")), reset}, {key(port(f.hub, "tick_sink")) + ".valid", clockValid},
      {key(port(f.hub, "tick_sink")) + ".bits.c0", clockIndex % 4 != 2},
      {key(port(f.hub, "tick_sink")) + ".bits.c1", clockIndex % 4 == 0 || clockIndex % 4 == 2}};
    for (unsigned stream = 0; stream < 2; ++stream) {
      auto in = key(port(f.hub, stream ? "right_sink" : "input_sink"));
      auto output = key(port(f.hub, stream ? "other_source" : "output_source"));
      hub.inputs[in + ".valid"] = (cycle + 3 * stream) % 7 != 3;
      hub.inputs[in + ".bits"] = payload(tokens[stream], stream);
      hub.inputs[output + ".ready"] = (cycle + 2 * stream) % 5 != 2;
    }
    if (reset) hub.resetHost();
    auto leftReady = hub.field("input_sink", "ready"), rightReady = hub.field("right_sink", "ready");
    auto clockReady = hub.field("tick_sink", "ready");
    out << "MSTATE " << associated << ' ' << cycle << " Hub 0 " << leftReady << ' ' << rightReady << ' '
      << hub.field("output_source", "valid") << ' ' << hub.field("other_source", "valid") << ' '
      << hub.field("output_source", "bits") << ' ' << hub.field("other_source", "bits") << ' '
      << hub.finishing() << ' ' << clockReady << ' ' << hub.gate("c0") << ' ' << hub.gate("c1") << ' '
      << hub.eval(hub.enabled.lookup("c0")) << ' ' << hub.eval(hub.enabled.lookup("c1")) << ' '
      << hub.eval(hub.fired.lookup("input")) << ' ' << hub.eval(hub.fired.lookup("right")) << ' '
      << hub.eval(hub.fired.lookup("output")) << ' ' << hub.eval(hub.fired.lookup("other")) << ' '
      << hub.state.at("state0") << ' ' << hub.state.at("state1") << '\n';
    hub.step();
    if (hub.inputs[key(port(f.hub,"input_sink")) + ".valid"] && leftReady) ++tokens[0];
    if (hub.inputs[key(port(f.hub,"right_sink")) + ".valid"] && rightReady) ++tokens[1];
    if (clockValid && clockReady) ++clockIndex;
    for (unsigned index = 0; index < 2; ++index) {
      auto &w = workers[index]; unsigned stream = index + 2;
      bool valid = (cycle + 3 * stream) % 7 != 3, ready = (cycle + 2 * stream) % 5 != 2;
      w.inputs = {{key(port(f.worker, "hostReset")), reset}, {key(port(f.worker, "input_sink")) + ".valid", valid},
        {key(port(f.worker, "input_sink")) + ".bits", payload(tokens[stream], stream)},
        {key(port(f.worker, "output_source")) + ".ready", ready}};
      if (reset) w.resetHost(); auto inputReady = w.field("input_sink", "ready");
      out << "MSTATE " << associated << ' ' << cycle << " Worker " << index << ' ' << inputReady << ' '
        << w.field("output_source", "valid") << ' ' << w.field("output_source", "bits") << ' ' << w.finishing() << ' '
        << w.gate("target") << ' ' << w.eval(w.enabled.lookup("target")) << ' ' << w.eval(w.fired.lookup("input")) << ' '
        << w.eval(w.fired.lookup("output")) << ' ' << w.state.at("state") << '\n';
      w.step(); if (valid && inputReady) ++tokens[stream];
    }
  }
  return trace;
}
void run(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 9; ++bad) {
    Fixture f(ctx, true); auto &later = f.plans[1];
    switch (bad) {
    case 0: f.plans.push_back(f.plans[0]); break;
    case 1: later.clocks[0].inputEnable = f.plans[0].clocks[0].inputEnable; break;
    case 2: later.channels[0].modelClockName = "c0"; break;
    case 3: later.dependencies[0].inputChannels = {"right"}; break;
    case 4: later.dependencies.clear(); break;
    case 5: later.channels.pop_back(); break;
    case 6: { OpBuilder b(f.worker.getBodyBlock(), f.worker.getBodyBlock()->end());
      Value ready = b.create<SubfieldOp>(f.worker.getLoc(), port(f.worker,"input_sink"), "ready");
      Value zero = b.create<ConstantOp>(f.worker.getLoc(), UIntType::get(&ctx,1), APInt(1,0));
      b.create<StrictConnectOp>(f.worker.getLoc(), ready, zero); b.create<StrictConnectOp>(f.worker.getLoc(), ready, zero); break; }
    case 7: std::swap(f.plans[0].clocks[0], f.plans[0].clocks[1]); break;
    case 8: later.channels[0].modelClockName.reset(); later.clockChannel = "input"; break;
    }
    auto before = dump(*f.root); std::string error;
    require(failed(rewriteFAMEModelControls(f.circuit, f.plans, error)) && !error.empty() && dump(*f.root) == before,
            "invalid ordered model plan accepted or mutated an earlier definition");
  }
  for (bool associated : {false,true}) {
    std::string expected;
    for (bool reversed : {false,true}) {
      Fixture f(ctx, associated); if (reversed) std::reverse(f.plans.begin(), f.plans.end());
      auto annotations = f.circuit->getAttr("rawAnnotations"); std::string error;
      SmallVector<Attribute> constraints;
      f.hub.walk([&](InstanceOp i) {
        if (i.getModuleName() == "AbstractClockGate") constraints.push_back(i->getAttr("goldengate.generatedClockConstraint"));
      });
      require(succeeded(rewriteFAMEModelControls(f.circuit, f.plans, error)), error);
      require(succeeded(verify(*f.root)), "model dispatch produced invalid IR");
      require(f.circuit->getAttr("rawAnnotations") == annotations, "model dispatch changed annotations");
      unsigned constraintIndex = 0;
      f.hub.walk([&](InstanceOp i) {
        if (i.getModuleName() == "AbstractClockGate")
          require(i->getAttr("goldengate.generatedClockConstraint") == constraints[constraintIndex++], "model dispatch changed XDC domain constraints");
      });
      require(constraintIndex == 2, "hub domain constraint coverage differs");
      unsigned workers = 0; f.circuit.walk([&](InstanceOp i) { if(i.getModuleName() == "Worker") { ++workers; require(i.getNumResults()==4 && i->hasAttr("test.preserved"), "shared worker ABI/metadata changed"); }});
      require(workers == 2, "shared workers lost");
      auto trace = simulate(f, associated);
      if (reversed) require(trace == expected, "model definition order changed independent behavior");
      else { expected = trace; llvm::outs() << trace; }
    }
  }
}
} // namespace
int main() { try { MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); run(ctx); return 0; }
  catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; } }
