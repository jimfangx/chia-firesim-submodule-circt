// See LICENSE for license details.
#include "goldengate/FAMEModelControls.h"
#include "goldengate/FAMEClockGate.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;
using namespace goldengate;

namespace {
bool isBit(Type type) {
  auto bit = dyn_cast<UIntType>(type);
  return bit && bit.getWidth() == 1;
}
bool hasHandshake(Type type) {
  auto bundle = dyn_cast<BundleType>(type);
  if (!bundle) return false;
  auto ready = bundle.getElement("ready"), valid = bundle.getElement("valid");
  auto bit = UIntType::get(type.getContext(), 1, false);
  return ready && valid && ready->isFlip && !valid->isFlip &&
         ready->type == bit && valid->type == bit;
}
bool isClockToken(Type type) {
  auto channel = dyn_cast<BundleType>(type);
  if (!channel || !hasHandshake(type)) return false;
  auto bits = channel.getElement("bits");
  if (!bits || bits->isFlip) return false;
  if (isa<ClockType>(bits->type)) return true;
  auto record = dyn_cast<BundleType>(bits->type);
  return record && !record.getElements().empty() &&
         llvm::all_of(record.getElements(), [](auto field) {
           return !field.isFlip && isa<ClockType>(field.type);
         });
}
Value port(FModuleOp model, StringRef name, Direction direction) {
  for (unsigned i = 0; i < model.getNumPorts(); ++i)
    if (model.getPortName(i) == name && model.getPortDirection(i) == direction)
      return model.getArgument(i);
  return {};
}
bool isLocal(Value value, FModuleOp model) {
  if (auto argument = dyn_cast<BlockArgument>(value))
    return argument.getOwner() == model.getBodyBlock();
  auto *op = value.getDefiningOp();
  return op && op->getParentOfType<FModuleOp>() == model;
}
struct PreparedPlan {
  const FAMEModelControlPlan *plan;
  SmallVector<FAMEFiredChannel> fired;
  SmallVector<std::string> inputs, outputs;
};
// Return all direct connections to a value or one of its decoupled fields.
SmallVector<Operation *> connections(FModuleOp model, Value destination,
                                     StringRef fieldName = {}) {
  SmallVector<Operation *> result;
  auto inspect = [&](Operation *op, Value value) {
    if (fieldName.empty()) {
      if (value == destination) result.push_back(op);
    } else if (auto field = value.getDefiningOp<SubfieldOp>();
               field && field.getInput() == destination &&
               field.getFieldName() == fieldName) {
      result.push_back(op);
    }
  };
  model.walk([&](ConnectOp op) { inspect(op, op.getDest()); });
  model.walk([&](StrictConnectOp op) { inspect(op, op.getDest()); });
  return result;
}
LogicalResult prepare(const FAMEModelControlPlan &plan, PreparedPlan &prepared,
                      std::string &error) {
  auto model = plan.model;
  auto reject = [&](const std::string &why) {
    error = "FAME model controls " + model.getName().str() + ": " + why;
    return failure();
  };
  Value hostClock = port(model, "hostClock", Direction::In);
  Value hostReset = port(model, "hostReset", Direction::In);
  if (!hostClock || !isa<ClockType>(hostClock.getType()) ||
      !hostReset || !isBit(hostReset.getType()))
    return reject("missing host clock or one-bit reset input");
  Value finishing;
  unsigned finishingWires = 0;
  model.walk([&](WireOp wire) {
    if (wire.getName() == "targetCycleFinishing") {
      finishing = wire.getResult(); ++finishingWires;
    }
  });
  if (!finishing || finishingWires != 1 || !isBit(finishing.getType()))
    return reject("requires one one-bit targetCycleFinishing wire");
  if (plan.clocks.empty()) return reject("missing model clock controls");
  bool virtualClock = plan.clockChannel.empty();
  if (virtualClock && plan.clocks.size() != 1)
    return reject("virtual clock requires one clock identity");

  Value clockPort;
  if (!virtualClock) {
    clockPort = port(model, plan.clockChannel + "_sink", Direction::In);
    if (!clockPort || !isClockToken(clockPort.getType()))
      return reject("missing Clock/ClockRecord sink " + plan.clockChannel);
  }
  FAMEClockEnableIndex enables;
  FAMEClockGateIndex gates;
  FAMEFiredRegisterIndex fired;
  if (failed(enables.collect(model, error)) || failed(gates.collect(model, error)) ||
      failed(fired.collect(model, error))) return failure();
  std::map<std::string, const FAMEHubClockControl *> controls;
  for (const auto &control : plan.clocks) {
    if (control.modelClockName.empty() ||
        !controls.emplace(control.modelClockName, &control).second)
      return reject("empty or duplicate model clock identity");
    if (!control.inputEnable || !control.outputEnable ||
        !isBit(control.inputEnable.getType()) || !isBit(control.outputEnable.getType()) ||
        !isLocal(control.inputEnable, model) || !isLocal(control.outputEnable, model))
      return reject("clock operands must be one-bit values in this definition");
    auto enabled = control.outputEnable.getDefiningOp<RegResetOp>();
    auto zero = enabled ? enabled.getResetValue().getDefiningOp<ConstantOp>() : ConstantOp();
    auto gate = gates.lookup(control.modelClockName);
    if (!enabled || enabled.getClockVal() != hostClock ||
        enabled.getResetSignal() != hostReset || !zero || !zero.getValue().isZero() ||
        enables.lookup(control.modelClockName) != control.outputEnable ||
        !gate || gate.getModuleName() != "AbstractClockGate" || gate.getNumResults() != 3 ||
        gate.getPortNameStr(0) != "I" || gate.getPortNameStr(1) != "CE" || gate.getPortNameStr(2) != "O" ||
        gate.getPortDirection(0) != Direction::In || gate.getPortDirection(1) != Direction::In ||
        gate.getPortDirection(2) != Direction::Out || !isa<ClockType>(gate.getResult(0).getType()) ||
        !isBit(gate.getResult(1).getType()) || !isa<ClockType>(gate.getResult(2).getType()))
      return reject("clock control lacks its host buffer/gate identity: " + control.modelClockName);
    if (virtualClock) {
      auto one = control.inputEnable.getDefiningOp<ConstantOp>();
      if (!one || !one.getValue().isOne())
        return reject("virtual clock raw enable must be constant one");
    }
  }
  if (!virtualClock) {
    auto payload = cast<BundleType>(clockPort.getType()).getElement("bits")->type;
    auto record = dyn_cast<BundleType>(payload);
    unsigned count = record ? record.getElements().size() : 1;
    if (plan.clocks.size() != count)
      return reject("clock control count disagrees with clock payload");
    for (auto [index, control] : llvm::enumerate(plan.clocks)) {
      auto cast = control.inputEnable.getDefiningOp<AsUIntPrimOp>();
      auto raw = cast ? cast.getInput().getDefiningOp<SubfieldOp>() : SubfieldOp();
      if (record) {
        if (!raw || raw.getFieldName() != record.getElements()[index].name.getValue())
          return reject("clock control order disagrees with ClockRecord fields");
        raw = raw.getInput().getDefiningOp<SubfieldOp>();
      }
      if (!raw || raw.getFieldName() != "bits" || raw.getInput() != clockPort)
        return reject("raw clock control is outside this model's clock channel");
    }
  }

  std::set<std::string> allNames, inputNames, outputNames;
  llvm::DenseSet<Value> covered;
  if (clockPort) { allNames.insert(plan.clockChannel); covered.insert(clockPort); }
  for (const auto &channel : plan.channels) {
    if (channel.name.empty() || !allNames.insert(channel.name).second)
      return reject("empty or duplicate data channel " + channel.name);
    bool input = channel.direction == Direction::In;
    auto value = port(model, channel.name + (input ? "_sink" : "_source"), channel.direction);
    if (!value || !hasHandshake(value.getType()))
      return reject("missing decoupled data port for " + channel.name);
    if (input && isClockToken(value.getType()))
      return reject("data channel is an unlisted clock sink: " + channel.name);
    covered.insert(value);
    (input ? inputNames : outputNames).insert(channel.name);
    (input ? prepared.inputs : prepared.outputs).push_back(channel.name);
    const FAMEHubClockControl *control = &plan.clocks.front();
    if (channel.modelClockName) {
      auto found = controls.find(*channel.modelClockName);
      if (found == controls.end())
        return reject("channel clock belongs to another domain/definition: " + channel.name);
      control = found->second;
    } else if (!virtualClock) {
      return reject("hub data channel lacks an associated clock: " + channel.name);
    }
    Value enable = !channel.modelClockName || input ? control->inputEnable : control->outputEnable;
    prepared.fired.push_back({channel.name, input, enable, bool(channel.modelClockName)});
    auto handshakes = connections(model, value, input ? "ready" : "valid");
    if (handshakes.size() > 1)
      return reject("multiple handshake connects for " + channel.name);
    if (Value state = fired.lookup(channel.name, false)) {
      auto reg = state.getDefiningOp<RegResetOp>();
      auto init = reg.getResetValue().getDefiningOp<ConstantOp>();
      if (!isBit(state.getType()) || reg.getClockVal() != hostClock ||
          reg.getResetSignal() != hostReset || !init ||
          init.getValue().getZExtValue() != unsigned(input && channel.modelClockName.has_value()) ||
          connections(model, state).size() != 1)
        return reject("invalid existing host fired state for " + channel.name);
    }
  }
  if (plan.channels.empty()) return reject("model has no data channels");
  for (unsigned i = 0; i < model.getNumPorts(); ++i)
    if (hasHandshake(model.getPortType(i)) && !covered.count(model.getArgument(i)))
      return reject("decoupled port omitted from cycle completion: " + model.getPortName(i).str());
  std::set<std::string> dependencyOutputs;
  for (const auto &dependency : plan.dependencies) {
    if (!outputNames.count(dependency.outputChannel) ||
        !dependencyOutputs.insert(dependency.outputChannel).second ||
        !dependency.unresolvedPorts.empty() || !dependency.unresolvedCauses.empty())
      return reject("duplicate, unresolved or foreign output dependency: " + dependency.outputChannel);
    std::set<std::string> dependencyInputs;
    for (const auto &name : dependency.inputChannels)
      if (!inputNames.count(name) || !dependencyInputs.insert(name).second)
        return reject("duplicate or foreign input dependency: " + name);
  }
  if (dependencyOutputs != outputNames)
    return reject("output dependency coverage is incomplete");
  auto finishingConnects = connections(model, finishing);
  auto clockConnects = clockPort ? connections(model, clockPort, "ready") : SmallVector<Operation *>();
  if (finishingConnects.size() > 1 || clockConnects.size() > 1)
    return reject("finishing or clock ready has multiple connects");
  auto *finish = finishingConnects.empty() ? nullptr : finishingConnects.front();
  auto *clock = clockConnects.empty() ? nullptr : clockConnects.front();
  if ((finish && clock && finish->getBlock() != clock->getBlock()) ||
      ((!finish || (!virtualClock && !clock)) &&
       ((finish && finish->getBlock() != model.getBodyBlock()) ||
        (clock && clock->getBlock() != model.getBodyBlock()))))
    return reject("finishing and clock ready require compatible module-body connects");
  return success();
}
} // namespace

LogicalResult goldengate::rewriteFAMEModelControls(
    CircuitOp circuit, ArrayRef<FAMEModelControlPlan> plans, std::string &error) {
  std::set<Operation *> seen;
  SmallVector<PreparedPlan> prepared;
  for (const auto &plan : plans) {
    auto model = plan.model;
    if (!model || model->getParentOp() != circuit.getOperation() ||
        !seen.insert(model.getOperation()).second) {
      error = "FAME model controls require distinct definitions in the supplied circuit";
      return failure();
    }
    prepared.push_back({&plan, {}, {}, {}});
    if (failed(prepare(plan, prepared.back(), error))) return failure();
  }
  // No model is changed until every definition has passed validation. Values
  // and port identities remain valid: these rewrites create only host state and
  // combinational equations, never rebuild a model or one of its instances.
  for (const auto &entry : prepared) {
    const auto &plan = *entry.plan;
    if (failed(ensureFAMEFiredRegisters(plan.model, entry.fired, error)) ||
        failed(rewriteFAMEOutputValids(plan.model, plan.dependencies, error)) ||
        failed(rewriteFAMEInputReadies(plan.model, entry.inputs, error)) ||
        failed(rewriteFAMEFiredStates(plan.model, entry.fired, error)) ||
        failed(rewriteFAMEFinishing(plan.model, entry.inputs, entry.outputs,
                                    plan.clockChannel, error)))
      return failure();
  }
  return success();
}
