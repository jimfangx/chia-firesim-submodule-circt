// See LICENSE for license details.
// Oracle: FAMETransform.targetClockMetadata and DefineAbstractClockGate.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEClockEnable.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  return parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "First" {
      firrtl.module @First(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock,
          in %other: !firrtl.clock) {
        %done = firrtl.wire : !firrtl.uint<1>
        %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
        %otherState = firrtl.reg %other : !firrtl.clock, !firrtl.uint<8>
      }
      firrtl.module @Second(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock) {
        %done = firrtl.wire : !firrtl.uint<1>
        %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
      }
    } }
  )mlir", &context);
}
Value driver(FModuleOp model, Value dest) {
  for (auto c : model.getOps<StrictConnectOp>())
    if (c.getDest() == dest)
      return c.getSrc();
  throw std::runtime_error("missing clock gate driver");
}
unsigned eval(Value value, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto it = values.find(value); it != values.end())
    return it->second;
  if (auto c = value.getDefiningOp<ConstantOp>())
    return c.getValue().getZExtValue();
  if (auto c = value.getDefiningOp<AsUIntPrimOp>())
    return eval(c.getInput(), values);
  if (auto c = value.getDefiningOp<ConstCastOp>())
    return eval(c.getInput(), values);
  if (auto c = value.getDefiningOp<AndPrimOp>())
    return eval(c.getLhs(), values) & eval(c.getRhs(), values);
  if (auto c = value.getDefiningOp<NotPrimOp>())
    return !eval(c.getInput(), values);
  if (auto c = value.getDefiningOp<MuxPrimOp>())
    return eval(c.getSel(), values) ? eval(c.getHigh(), values)
                                      : eval(c.getLow(), values);
  throw std::runtime_error("unsupported clock control expression");
}

void collisionControls(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "Model" {
      firrtl.module @Model(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>, in %target: !firrtl.clock,
          in %token: !firrtl.uint<1>, in %other: !firrtl.clock,
          in %other_enabled: !firrtl.uint<1>) {
        %done = firrtl.wire : !firrtl.uint<1>
        %zero = firrtl.constant 0 : !firrtl.uint<1>
        %one = firrtl.constant 1 : !firrtl.uint<1>
        %target_enabled_0 = firrtl.node %one : !firrtl.uint<1>
        %state = firrtl.reg %target : !firrtl.clock, !firrtl.uint<8>
        %otherState = firrtl.reg %other : !firrtl.clock, !firrtl.uint<8>
      }
    } }
  )mlir", &context);
  require(bool(root), "collision fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto model = *circuit.getOps<FModuleOp>().begin();
  auto finishing = *model.getOps<WireOp>().begin();
  finishing.setName("targetCycleFinishing");
  OpBuilder b(&model.getBodyBlock()->front());
  auto bit = UIntType::get(&context, 1);
  Value zero = b.create<ConstantOp>(model.getLoc(), bit, APInt(1, 0));
  Value one = b.create<ConstantOp>(model.getLoc(), bit, APInt(1, 1));
  auto target = b.create<RegResetOp>(model.getLoc(), bit,
      model.getArgument(0), model.getArgument(1), zero, "target_enabled");
  b.setInsertionPointToEnd(model.getBodyBlock());
  b.create<ConnectOp>(model.getLoc(), target.getResult(), one);
  std::string error;
  require(succeeded(goldengate::addFAMEClockEnable(
              model, "target", model.getArgument(3), error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockEnable(
              model, "other", {}, error)), error.c_str());
  goldengate::FAMEClockEnableIndex enables;
  require(succeeded(enables.collect(model, error)), error.c_str());
  auto enabled = enables.lookup("target", false).getDefiningOp<RegResetOp>();
  auto other = enables.lookup("other", false).getDefiningOp<RegResetOp>();
  require(enabled && enabled.getName() == "target_enabled_1" &&
              other && other.getName() == "other_enabled_0" &&
              enabled != target && !target->hasAttr(goldengate::fameClockEnableAttr),
          "clock enable collided with target state, node or port");
  llvm::outs() << "IDENTITY target " << enabled.getName() << '\n'
               << "IDENTITY other " << other.getName() << '\n';
  auto before = dump(*root);
  require(failed(goldengate::addFAMEClockEnable(
              model, "target", model.getArgument(3), error)) &&
              dump(*root) == before, "duplicate clock creation mutated state");
  enabled.setName("renamed_host_enable");
  // Persist identity through the boundary consumed by subsequent passes.
  root = parseSourceString<ModuleOp>(dump(*root), &context);
  require(bool(root), "clock identity round trip failed");
  circuit = *root->getOps<CircuitOp>().begin();
  model = *circuit.getOps<FModuleOp>().begin();
  require(succeeded(enables.collect(model, error)), error.c_str());
  enabled = enables.lookup("target").getDefiningOp<RegResetOp>();
  require(enabled && enabled.getName() == "renamed_host_enable",
          "clock identity was lost after renaming/serialization");
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, model, "target", error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, model, "other", error)), error.c_str());
  for (auto gate : model.getOps<InstanceOp>()) {
    auto clock = gate.getName().drop_back(7);
    auto reg = enables.lookup(clock).getDefiningOp<RegResetOp>();
    auto ce = driver(model, gate.getResult(1));
    for (unsigned mask = 0; mask < 16; ++mask) {
      unsigned reset = mask & 1, done = (mask >> 1) & 1;
      unsigned old = (mask >> 2) & 1, token = (mask >> 3) & 1;
      llvm::DenseMap<Value, unsigned> values{
          {model.getArgument(1), reset}, {model.getArgument(3), token},
          {reg.getResult(), old}};
      for (auto wire : model.getOps<WireOp>())
        if (wire.getName() == "targetCycleFinishing")
          values[wire.getResult()] = done;
      unsigned expectedToken = clock == "target" ? token : 1;
      require((reset ? eval(reg.getResetValue(), values)
                       : eval(driver(model, reg.getResult()), values)) ==
                  (reset ? 0 : done ? expectedToken : old) &&
                  eval(ce, values) == (old & done & !reset),
              "clock identity changed SFC buffer/gate behavior");
    }
  }
  require(succeeded(verify(*root)), "collision clock circuit failed verification");
  enabled->removeAttr(goldengate::fameClockEnableAttr);
  require(succeeded(enables.collect(model, error)) && !enables.lookup("target"),
          "native model fell back to colliding target register");
  enabled->setAttr(goldengate::fameClockEnableAttr,
                    StringAttr::get(&context, "target"));
  // Target state with the same schema remains untouched and cannot be used as
  // host state when a native identity for another clock is present.
  for (auto reg : model.getOps<RegResetOp>())
    if (reg.getName() == "target_enabled") {
      require(!reg->hasAttr(goldengate::fameClockEnableAttr), "target state retagged");
      bool preserved = false;
      for (auto c : model.getOps<ConnectOp>())
        if (c.getDest() == reg.getResult())
          preserved = eval(c.getSrc(), llvm::DenseMap<Value, unsigned>()) == 1;
      require(preserved, "target clock-name state changed");
      reg->setAttr(goldengate::fameClockEnableAttr,
                    StringAttr::get(&context, "target"));
      before = dump(*root);
      require(failed(enables.collect(model, error)) &&
                  failed(goldengate::addFAMEClockEnable(model, "third", {}, error)) &&
                  failed(goldengate::addFAMEClockGate(circuit, model, "target", error)) &&
                  dump(*root) == before, "duplicate clock identity accepted or mutated IR");
    }
  llvm::outs() << "Matched 32 collision/renamed real/virtual clock reset, hold, "
                  "capture and gate cases; preserved target state\n";
  for (unsigned rejection = 0; rejection < 6; ++rejection) {
    auto badRoot = fixture(context);
    auto badCircuit = *badRoot->getOps<CircuitOp>().begin();
    auto badModel = *badCircuit.getOps<FModuleOp>().begin();
    auto done = *badModel.getOps<WireOp>().begin();
    done.setName("targetCycleFinishing");
    require(succeeded(goldengate::addFAMEClockEnable(
                badModel, "target", {}, error)), error.c_str());
    auto flag = goldengate::lookupFAMEClockEnable(
        badModel, "target", error).getDefiningOp<RegResetOp>();
    OpBuilder invalid(flag);
    Value one = invalid.create<ConstantOp>(flag.getLoc(), bit, APInt(1, 1));
    if (rejection == 0)
      flag->setAttr(goldengate::fameClockEnableAttr, invalid.getI32IntegerAttr(0));
    if (rejection == 1)
      flag->setAttr(goldengate::fameClockEnableAttr, invalid.getStringAttr(""));
    if (rejection == 2)
      done->setAttr(goldengate::fameClockEnableAttr, invalid.getStringAttr("other"));
    if (rejection == 3) flag.getResetValueMutable().assign(one);
    if (rejection == 4) flag.getClockValMutable().assign(badModel.getArgument(2));
    if (rejection == 5) flag.getResetSignalMutable().assign(one);
    before = dump(*badRoot);
    require(failed(goldengate::addFAMEClockGate(
                badCircuit, badModel, "target", error)) && !error.empty() &&
                dump(*badRoot) == before,
            "malformed clock identity/host controls accepted or mutated IR");
  }
  llvm::outs() << "Rejected six malformed clock identities/host-control contracts\n";
}

void rocketBoundary(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "Rocket boundary parse failed");
  FModuleOp model;
  root->walk([&](FModuleOp op) { if (op.getName() == "FireSim") model = op; });
  require(bool(model), "Rocket FireSim model missing");
  std::string error;
  auto oldValue = goldengate::lookupFAMEClockEnable(
      model, "clockBridge_clocks_0", error);
  require(bool(oldValue), error.c_str());
  auto old = oldValue.getDefiningOp<RegResetOp>();
  auto oldConnect = [&]() -> StrictConnectOp {
    for (auto c : model.getOps<StrictConnectOp>())
      if (c.getDest() == oldValue) return c;
    return {};
  }();
  require(bool(oldConnect), "Rocket buffered enable driver missing");
  auto mux = oldConnect.getSrc().getDefiningOp<MuxPrimOp>();
  require(bool(mux), "Rocket buffered enable mux missing");
  Value token = mux.getHigh(), finishing = mux.getSel();
  old.setName("oracle_old_enable");
  old->removeAttr(goldengate::fameClockEnableAttr);
  require(succeeded(goldengate::addFAMEClockEnable(
              model, "clockBridge_clocks_0", token, error)), error.c_str());
  auto value = goldengate::lookupFAMEClockEnable(
      model, "clockBridge_clocks_0", error);
  auto reg = value.getDefiningOp<RegResetOp>();
  require(reg && reg.getName() == "clockBridge_clocks_0_enabled",
          "Rocket enable name differs from SFC");
  oldConnect.erase();
  oldValue.replaceAllUsesWith(value);
  old.erase();
  InstanceOp gate;
  for (auto instance : model.getOps<InstanceOp>())
    if (instance.getName() == "clockBridge_clocks_0_buffer") gate = instance;
  require(bool(gate), "Rocket gate missing");
  auto tokenCast = token.getDefiningOp<AsUIntPrimOp>();
  require(bool(tokenCast), "Rocket token cast missing");
  for (unsigned mask = 0; mask < 16; ++mask) {
    unsigned reset = mask & 1, done = (mask >> 1) & 1;
    unsigned oldEnable = (mask >> 2) & 1, nextToken = (mask >> 3) & 1;
    llvm::DenseMap<Value, unsigned> values{
        {reg.getResetSignal(), reset}, {finishing, done},
        {value, oldEnable}, {tokenCast.getInput(), nextToken}};
    unsigned next = reset ? eval(reg.getResetValue(), values)
                           : eval(driver(model, value), values);
    unsigned ce = eval(driver(model, gate.getResult(1)), values);
    require(next == (reset ? 0 : done ? nextToken : oldEnable) &&
                ce == (oldEnable & done & !reset), "Rocket enable/gate differs from SFC");
    llvm::outs() << "CLOCK " << reset << ' ' << done << ' ' << oldEnable << ' '
                 << nextToken << ' ' << next << ' ' << ce << '\n';
  }
  require(driver(model, gate.getResult(0)) == reg.getClockVal() &&
              succeeded(verify(*root)), "Rocket host clock or IR changed");
  llvm::outs() << "BOUNDARY " << reg.getName() << " reset=0 gate="
               << gate.getName() << "/O\n";
}

void collisionBoundary(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "collision control boundary parse failed");
  FModuleOp model;
  root->walk([&](FModuleOp op) { if (op.getName() == "Model") model = op; });
  require(bool(model), "collision model missing");
  std::string error;
  auto value = goldengate::lookupFAMEClockEnable(model, "target", error);
  require(value && value.getDefiningOp<RegResetOp>().getName() == "target_enabled_1",
          "CLI clock-enable allocation differs from SFC namespace");
  RegResetOp target, fired;
  WireOp finishing;
  InstanceOp gate;
  for (auto reg : model.getOps<RegResetOp>()) {
    if (reg.getName() == "target_enabled") target = reg;
    if (reg.getName() == "output_fired_0") fired = reg;
  }
  for (auto wire : model.getOps<WireOp>())
    if (wire.getName() == "targetCycleFinishing") finishing = wire;
  for (auto instance : model.getOps<InstanceOp>())
    if (instance.getName() == "target_buffer") gate = instance;
  require(target && fired && finishing && gate &&
              !target->hasAttr(goldengate::fameClockEnableAttr),
          "CLI dropped target state or generated clock controls");
  Value targetDriver = driver(model, target.getResult());
  require(targetDriver && eval(targetDriver, llvm::DenseMap<Value, unsigned>()) == 1,
          "CLI modified colliding target state");
  for (unsigned reset = 0; reset < 2; ++reset)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
      llvm::DenseMap<Value, unsigned> values{
          {value, enabled}, {target.getResult(), 1},
          {finishing.getResult(), 1}, {target.getResetSignal(), reset}};
      require(eval(driver(model, gate.getResult(1)), values) == (enabled & !reset) &&
                  eval(driver(model, fired.getResult()), values) == !enabled,
              "CLI gate or output fired-state consumed colliding target state");
    }
  require(succeeded(verify(*root)), "CLI collision controls failed verification");
  llvm::outs() << "Matched CLI collision clock gate and output fired-state consumers\n";
}
void run(MLIRContext &context) {
  auto root = fixture(context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  SmallVector<FModuleOp> models(circuit.getOps<FModuleOp>());
  for (auto model : models) {
    auto finishing = *model.getOps<WireOp>().begin();
    finishing.setName("targetCycleFinishing");
    std::string error;
    require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)),
            error.c_str());
  }
  std::string error;
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[0], "target", error)), error.c_str());
  auto gate = *circuit.getOps<FExtModuleOp>().begin();
  // Reuse must preserve the definition, including externally supplied metadata.
  OpBuilder b(&context);
  gate->setAttr("test.preserved", b.getStringAttr("shared definition"));
  gate.setPortSymbolsAttr(0, circt::hw::InnerSymAttr::get(b.getStringAttr("input_id")));
  auto anno = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Gate"))});
  gate.setAnnotationsAttr(b.getArrayAttr({anno}));
  std::string definition = dump(gate);
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[1], "target", error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockEnable(
              models[0], "other", {}, error)), error.c_str());
  require(succeeded(goldengate::addFAMEClockGate(
              circuit, models[0], "other", error)), error.c_str());
  require(dump(gate) == definition &&
              std::distance(circuit.getOps<FExtModuleOp>().begin(),
                            circuit.getOps<FExtModuleOp>().end()) == 1,
          "shared gate definition was changed or duplicated");
  unsigned count = 0;
  for (auto model : models) {
    for (auto instance : model.getOps<InstanceOp>()) {
      ++count;
      require(driver(model, instance.getResult(0)) == model.getArgument(0),
              "gate uses another model's host clock");
      auto ce = driver(model, instance.getResult(1)).getDefiningOp<AndPrimOp>();
      auto active = ce.getLhs().getDefiningOp<AndPrimOp>();
      auto reset = ce.getRhs().getDefiningOp<NotPrimOp>();
      auto enabled = active.getLhs().getDefiningOp<RegResetOp>();
      auto finishing = active.getRhs().getDefiningOp<WireOp>();
      require(enabled && finishing && reset &&
                  enabled->getParentOp() == model && finishing->getParentOp() == model &&
                  reset.getInput() == model.getArgument(1) &&
                  enabled.getName() ==
                      (instance.getName().drop_back(7) + "_enabled").str(),
              "gate shares enable, completion or reset controls");
      for (auto state : model.getOps<RegOp>()) {
        auto name = state.getName();
        if ((name == "state" && instance.getName() == "target_buffer") ||
            (name == "otherState" && instance.getName() == "other_buffer"))
          require(state.getClockVal() == instance.getResult(2),
                  "target state did not receive its domain's gate output");
      }
    }
  }
  require(count == 3 && succeeded(verify(*root)), "shared gate circuit failed verification");

  // Each invalid symbol/schema must fail without changing the circuit.
  for (unsigned rejection = 0; rejection < 8; ++rejection) {
    auto badRoot = fixture(context);
    auto badCircuit = *badRoot->getOps<CircuitOp>().begin();
    auto model = *badCircuit.getOps<FModuleOp>().begin();
    (*model.getOps<WireOp>().begin()).setName("targetCycleFinishing");
    require(succeeded(goldengate::addFAMEClockEnable(model, "target", {}, error)),
            error.c_str());
    b.setInsertionPointToEnd(badCircuit.getBodyBlock());
    SmallVector<PortInfo> ports{
      {b.getStringAttr("I"), ClockType::get(&context), Direction::In},
      {b.getStringAttr("CE"), UIntType::get(&context, 1), Direction::In},
      {b.getStringAttr("O"), ClockType::get(&context), Direction::Out}};
    if (rejection == 1) ports[1].type = UIntType::get(&context, 2);
    if (rejection == 2) ports[0].direction = Direction::Out;
    if (rejection == 3) ports[0].name = b.getStringAttr("wrong");
    if (rejection == 4) ports.pop_back();
    if (rejection == 0)
      b.create<FModuleOp>(model.getLoc(), b.getStringAttr("AbstractClockGate"),
          ConventionAttr::get(&context, Convention::Internal), ports);
    else {
      auto invalid = b.create<FExtModuleOp>(model.getLoc(), b.getStringAttr("AbstractClockGate"),
          ConventionAttr::get(&context, rejection == 6 ? Convention::Scalarized : Convention::Internal),
          ports, rejection == 5 ? "DifferentGate" : "AbstractClockGate");
      if (rejection == 7)
        invalid.setParametersAttr(b.getArrayAttr({ParamDeclAttr::get(
            &context, b.getStringAttr("mode"), b.getI32Type(), b.getI32IntegerAttr(1))}));
    }
    auto before = dump(*badRoot);
    require(failed(goldengate::addFAMEClockGate(badCircuit, model, "target", error)) &&
                !error.empty(), "incompatible gate definition was accepted");
    require(dump(*badRoot) == before, "rejected gate definition mutated IR");
  }
  llvm::outs() << "Shared one gate definition across two models and three domains; "
                  "preserved metadata and independent controls; rejected eight incompatible definitions\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    collisionControls(context);
    if (argc == 2)
      rocketBoundary(context, argv[1]);
    else if (argc == 3 && llvm::StringRef(argv[1]) == "--collision-controls")
      collisionBoundary(context, argv[2]);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
