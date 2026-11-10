// See LICENSE for license details.
// Compare emitted non-hub control/state equations with the preserved SFC
// FAMEVirtualModelClockOracle.scala under independent stalls and target reset.
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
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
FModuleOp lookup(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  throw std::runtime_error("missing model");
}
std::string key(Value value) {
  if (auto field = value.getDefiningOp<SubfieldOp>())
    return key(field.getInput()) + "." + field.getFieldName().str();
  return std::to_string(reinterpret_cast<uintptr_t>(value.getAsOpaquePointer()));
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned rejection = 0) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module { firrtl.circuit "Top" {
    firrtl.module @Top(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %target0: !firrtl.clock, in %target1: !firrtl.clock) {}
    firrtl.module private @Worker(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %target: !firrtl.clock,
      in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
      out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
      %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
      %zero = firrtl.constant 0 : !firrtl.uint<1>
      firrtl.strictconnect %targetCycleFinishing, %zero : !firrtl.uint<1>
    }
  } })mlir", &ctx);
  require(bool(root), "virtual clock fixture parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = lookup(circuit, "Top"), model = lookup(circuit, "Worker");
  OpBuilder b(model.getBodyBlock(), model.getBodyBlock()->end()); auto loc = model.getLoc();
  auto field = [&](Value v, StringRef name) -> Value { return b.create<SubfieldOp>(loc, v, name); };
  Value data = field(model.getArgument(3), "bits");
  Value reset = b.create<BitsPrimOp>(loc, data, 7, 7);
  Value zero = b.create<ConstantOp>(loc, UIntType::get(&ctx, 8), APInt(8, 0));
  auto state = b.create<RegResetOp>(loc, UIntType::get(&ctx, 8), model.getArgument(2), reset, zero, "state");
  Value increment = b.create<BitsPrimOp>(loc, data, 6, 0);
  Value sum = b.create<AddPrimOp>(loc, state.getResult(), increment);
  Value result = b.create<BitsPrimOp>(loc, sum, 7, 0);
  b.create<StrictConnectOp>(loc, state.getResult(), result);
  b.create<StrictConnectOp>(loc, field(model.getArgument(4), "bits"), result);
  if (rejection == 1) {
    model.insertPorts({{unsigned(model.getNumPorts()), PortInfo(b.getStringAttr("secondTarget"), ClockType::get(&ctx), Direction::In)}});
  } else if (rejection == 2) {
    auto attrs = model.getPortAnnotationsAttr();
    SmallVector<Attribute> changed(attrs.begin(), attrs.end());
    if (changed.empty()) changed.assign(model.getNumPorts(), b.getArrayAttr({}));
    changed[2] = b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Clock"))})});
    model.setPortAnnotationsAttr(b.getArrayAttr(changed));
  } else if (rejection == 3) {
    model.setPortSymbolsAttr(2, circt::hw::InnerSymAttr::get(b.getStringAttr("clock_symbol")));
  } else if (rejection == 4) {
    b.setInsertionPointToEnd(circuit.getBodyBlock());
    b.create<FModuleOp>(loc, b.getStringAttr("AbstractClockGate"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  for (unsigned index = 0; index < 2; ++index) {
    auto instance = b.create<InstanceOp>(loc, model, "worker" + std::to_string(index));
    instance->setAttr("test.preserved", b.getStringAttr("instance metadata"));
    b.create<StrictConnectOp>(loc, instance.getResult(0), top.getArgument(0));
    b.create<StrictConnectOp>(loc, instance.getResult(1), top.getArgument(1));
    b.create<StrictConnectOp>(loc, instance.getResult(2), top.getArgument(2 + index));
    if (rejection == 5 && index == 1) {
      auto wire = b.create<WireOp>(loc, ClockType::get(&ctx), "unsafeClockRead");
      b.create<StrictConnectOp>(loc, wire.getResult(), instance.getResult(2));
    } else if (rejection == 6 && index == 1) {
      auto attrs = instance.getPortAnnotationsAttr(); SmallVector<Attribute> changed(attrs.begin(), attrs.end());
      if (changed.empty()) changed.assign(instance.getNumResults(), b.getArrayAttr({}));
      changed[2] = b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Clock"))})});
      instance.setPortAnnotationsAttr(b.getArrayAttr(changed));
    }
  }
  return root;
}
struct Interpreter {
  FModuleOp model;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> state, inputs;
  goldengate::FAMEClockGateIndex gates;
  goldengate::FAMEFiredRegisterIndex fired;
  Value enabled;
  explicit Interpreter(FModuleOp m) : model(m) {
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple interpreter drivers");
    for (auto r : m.getOps<RegResetOp>()) state[r.getName().str()] = r.getName() == "state" ? 37 : 0;
    std::string error;
    require(succeeded(gates.collect(m, error)) && succeeded(fired.collect(m, error)), error);
    enabled = goldengate::lookupFAMEClockEnable(m, "target", error); require(bool(enabled), error);
  }
  uint64_t eval(Value v) {
    uint64_t value = 0;
    if (auto found = inputs.find(key(v)); found != inputs.end()) return found->second;
    if (auto r = v.getDefiningOp<RegResetOp>()) return state.at(r.getName().str());
    if (auto c = v.getDefiningOp<ConstantOp>()) return c.getValue().getZExtValue();
    if (auto found = drivers.find(key(v)); found != drivers.end()) return eval(found->second);
    if (auto p = v.getDefiningOp<NotPrimOp>()) value = ~eval(p.getInput());
    else if (auto p = v.getDefiningOp<AndPrimOp>()) value = eval(p.getLhs()) & eval(p.getRhs());
    else if (auto p = v.getDefiningOp<OrPrimOp>()) value = eval(p.getLhs()) | eval(p.getRhs());
    else if (auto p = v.getDefiningOp<AddPrimOp>()) value = eval(p.getLhs()) + eval(p.getRhs());
    else if (auto p = v.getDefiningOp<BitsPrimOp>()) value = eval(p.getInput()) >> p.getLo();
    else if (auto p = v.getDefiningOp<MuxPrimOp>()) value = eval(eval(p.getSel()) ? p.getHigh() : p.getLow());
    else if (auto p = v.getDefiningOp<AsUIntPrimOp>()) value = eval(p.getInput());
    else throw std::runtime_error("unsupported emitted virtual-clock equation");
    auto type = dyn_cast<UIntType>(v.getType());
    auto width = type ? type.getWidth() : std::nullopt;
    require(width && *width > 0 && *width < 64, "interpreter integer width");
    return value & ((uint64_t(1) << *width) - 1);
  }
  Value arg(StringRef name) {
    for (unsigned i = 0; i < model.getNumPorts(); ++i) if (model.getPortName(i) == name) return model.getArgument(i);
    throw std::runtime_error("interpreter port missing");
  }
  uint64_t field(StringRef port, StringRef name) {
    auto root = arg(port); auto found = drivers.find(key(root) + "." + name.str());
    if (found != drivers.end()) return eval(found->second);
    return inputs.at(key(root) + "." + name.str());
  }
  uint64_t finishing() {
    for (auto wire : model.getOps<WireOp>()) if (wire.getName() == "targetCycleFinishing") return eval(wire.getResult());
    throw std::runtime_error("finishing missing");
  }
  uint64_t gateEnable() { return eval(gates.lookup("target", false).getResult(1)); }
  void step() {
    auto next = state;
    for (auto r : model.getOps<RegResetOp>()) {
      bool advance = r.getClockVal() == arg("hostClock") || gateEnable();
      if (advance) next[r.getName().str()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    }
    state = std::move(next);
  }
};
void run(MLIRContext &ctx) {
  for (unsigned reject = 1; reject <= 6; ++reject) {
    auto root = fixture(ctx, reject); auto c = *root->getOps<CircuitOp>().begin(); auto m = lookup(c, "Worker");
    std::string before = dump(*root), error;
    require(!goldengate::constructFAMEVirtualClockControls(c, m, "target", error) && !error.empty() && dump(*root) == before,
            "rejected virtual clock constructor mutated model, gate definition, or another instance");
  }
  for (bool associated : {false, true}) {
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto m = lookup(c, "Worker");
    std::string error;
    auto control = goldengate::constructFAMEVirtualClockControls(c, m, "target", error);
    require(bool(control), error);
    require(control->inputEnable.getDefiningOp<ConstantOp>().getValue().isOne(), "virtual token is not constant one");
    SmallVector<goldengate::FAMEFiredChannel> channels{
      {"input", true, control->inputEnable, associated},
      {"output", false, associated ? control->outputEnable : control->inputEnable, associated}};
    goldengate::LocalChannelDependency dependency; dependency.outputChannel = "output"; dependency.inputChannels = {"input"};
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, channels, error)), error);
    require(succeeded(goldengate::rewriteFAMEOutputValids(m, {dependency}, error)), error);
    require(succeeded(goldengate::rewriteFAMEInputReadies(m, {"input"}, error)), error);
    require(succeeded(goldengate::rewriteFAMEFiredStates(m, channels, error)), error);
    require(succeeded(goldengate::rewriteFAMEFinishing(m, {"input"}, {"output"}, "", error)), error);
    require(succeeded(verify(*root)), "complete virtual controls invalid IR");
    unsigned instances = 0;
    c.walk([&](InstanceOp instance) {
      if (instance.getModuleName() != "Worker") return;
      ++instances; require(instance.getNumResults() == 4 && instance->hasAttr("test.preserved"), "shared-instance clock removal lost ABI/metadata");
    });
    require(instances == 2, "repeated virtual model instances lost");
    m.walk([&](Operation *op) {
      require(!op->hasAttr("goldengate.generatedClockConstraint"), "non-hub acquired hub XDC hints");
    });
    auto before = dump(*root); error.clear();
    require(!goldengate::constructFAMEVirtualClockControls(c, m, "target", error) && dump(*root) == before, "second virtual construction mutated model");
    Interpreter workers[]{Interpreter(m), Interpreter(m)};
    unsigned tokenIndex[2]{};
    for (unsigned cycle = 0; cycle < 96; ++cycle) for (unsigned index = 0; index < 2; ++index) {
      auto &sim = workers[index];
      bool reset = cycle < 2 || cycle == 40 || cycle == 41;
      bool valid = (cycle + 3 * index) % 7 != 3;
      bool ready = (cycle + 2 * index) % 5 != 2;
      unsigned n = tokenIndex[index];
      unsigned data = (n == 0 || n % 11 == 7) ? 128 : (n * 17 + index * 9) % 127;
      sim.inputs = {{key(sim.arg("hostReset")), reset}, {key(sim.arg("input_sink")) + ".valid", valid},
        {key(sim.arg("input_sink")) + ".bits", data}, {key(sim.arg("output_source")) + ".ready", ready}};
      if (reset) for (auto r : m.getOps<RegResetOp>()) if (r.getClockVal() == sim.arg("hostClock")) sim.state[r.getName().str()] = sim.eval(r.getResetValue());
      auto inputReady = sim.field("input_sink", "ready");
      llvm::outs() << "VSTATE " << (associated ? 1 : 0) << ' ' << cycle << ' ' << index << ' '
        << inputReady << ' ' << sim.field("output_source", "valid") << ' ' << sim.field("output_source", "bits") << ' '
        << sim.finishing() << ' ' << sim.gateEnable() << ' ' << sim.eval(sim.enabled) << ' '
        << sim.eval(sim.fired.lookup("input")) << ' ' << sim.eval(sim.fired.lookup("output")) << ' ' << sim.state.at("state") << '\n';
      sim.step(); if (valid && inputReady) ++tokenIndex[index];
    }
  }
}
} // namespace
int main() {
  try { MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); run(ctx); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
