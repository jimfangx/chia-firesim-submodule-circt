// See LICENSE for license details.
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEFiredRegister.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct ChannelValues {
  Value port;
  Value fired;
};

bool isBit(Type type) {
  auto uint = dyn_cast<UIntType>(type);
  return uint && uint.getWidth() == 1;
}

bool hasDecoupledFields(Value port) {
  auto bundle = dyn_cast<BundleType>(port.getType());
  if (!bundle)
    return false;
  auto ready = bundle.getElementIndex("ready");
  auto valid = bundle.getElementIndex("valid");
  return ready && valid && bundle.getElements()[*ready].isFlip &&
         !bundle.getElements()[*valid].isFlip &&
         isBit(bundle.getElements()[*ready].type) &&
         isBit(bundle.getElements()[*valid].type);
}

bool hasClockToken(Value port) {
  auto bundle = dyn_cast<BundleType>(port.getType());
  if (!bundle || !hasDecoupledFields(port))
    return false;
  auto bits = bundle.getElementIndex("bits");
  if (!bits || bundle.getElements()[*bits].isFlip)
    return false;
  auto payload = bundle.getElements()[*bits].type;
  if (isa<ClockType>(payload))
    return true;
  // HasModelPort packs several scalar Clock ports into a ClockRecord. The
  // whole record is one clock token, with one valid/ready handshake. Require
  // the same passive, nonempty set of Clock leaves produced by that boundary;
  // a mixed data bundle must not silently become the completion clock.
  auto record = dyn_cast<BundleType>(payload);
  return record && !record.getElements().empty() &&
         llvm::all_of(record.getElements(), [](BundleType::BundleElement e) {
           return !e.isFlip && isa<ClockType>(e.type);
         });
}
} // namespace

LogicalResult goldengate::rewriteFAMEFinishing(
    FModuleOp module, llvm::ArrayRef<std::string> inputChannels,
    llvm::ArrayRef<std::string> outputChannels, llvm::StringRef clockChannel,
    std::string &error) {
  // Scala BinaryBooleanOp.reduce rejects an empty data-channel reduction;
  // a real or virtual clock token is applied only after this reduction.
  if (inputChannels.empty() && outputChannels.empty()) {
    error = "FAME model has no data channels";
    return failure();
  }
  Value finishing;
  module.walk([&](WireOp op) {
    if (op.getName() == "targetCycleFinishing")
      finishing = op.getResult();
  });
  if (!finishing || !isBit(finishing.getType())) {
    error = "missing one-bit targetCycleFinishing wire";
    return failure();
  }

  std::map<std::string, unsigned> ports;
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i)
    ports.emplace(module.getPortName(i).str(), i);
  auto lookupPort = [&](const std::string &name, Direction direction) -> Value {
    auto found = ports.find(name);
    if (found == ports.end() || module.getPortDirection(found->second) != direction)
      return {};
    return module.getBodyBlock()->getArgument(found->second);
  };
  // VirtualClockChannel has no model port, is always valid, and setReady
  // emits no statement. An empty channel name explicitly selects that case.
  bool virtualClock = clockChannel.empty();
  Value clockPort;
  if (!virtualClock)
    clockPort = lookupPort((clockChannel + "_sink").str(), Direction::In);
  if (!virtualClock && (!clockPort || !hasClockToken(clockPort))) {
    error = "missing Clock/ClockRecord target clock sink " + clockChannel.str();
    return failure();
  }

  FAMEFiredRegisterIndex firedRegisters;
  if (failed(firedRegisters.collect(module, error)))
    return failure();
  // Scala selects a clock channel whose payload ports all have ClockType;
  // counting it again as a data input would make its valid gate appear twice.
  std::set<std::string> seen;
  if (!virtualClock)
    seen.insert(clockChannel.str());
  SmallVector<ChannelValues> inputs, outputs;
  auto collect = [&](llvm::ArrayRef<std::string> names, bool input,
                     SmallVectorImpl<ChannelValues> &values) -> bool {
    for (const auto &name : names) {
      if (!seen.insert(name).second) {
        error = "duplicate FAME channel " + name;
        return false;
      }
      Value port = lookupPort(name + (input ? "_sink" : "_source"),
                              input ? Direction::In : Direction::Out);
      if (!port || !hasDecoupledFields(port)) {
        error = "missing decoupled FAME port for " + name;
        return false;
      }
      if (input && hasClockToken(port)) {
        error = "Clock-typed sink requires an explicit target clock channel: " + name;
        return false;
      }
      Value fired = firedRegisters.lookup(name);
      if (!fired || !isBit(fired.getType()) ||
          !fired.getDefiningOp<RegResetOp>()) {
        error = "missing one-bit fired register for " + name;
        return false;
      }
      values.push_back({port, fired});
    }
    return true;
  };
  if (!collect(outputChannels, false, outputs) ||
      !collect(inputChannels, true, inputs))
    return failure();

  // The clock token and every converted data channel must participate in
  // cycle completion.  A missing channel would otherwise leave its ready or
  // valid handshake outside the finishing equation while producing valid IR.
  llvm::DenseSet<Value> coveredPorts;
  if (clockPort)
    coveredPorts.insert(clockPort);
  for (const auto &channel : inputs)
    coveredPorts.insert(channel.port);
  for (const auto &channel : outputs)
    coveredPorts.insert(channel.port);
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i) {
    Value port = module.getBodyBlock()->getArgument(i);
    if (!hasDecoupledFields(port) || coveredPorts.count(port))
      continue;
    error = "decoupled FAME port omitted from cycle completion: " +
            module.getPortName(i).str();
    return failure();
  }

  Operation *finishingConnect = nullptr, *clockReadyConnect = nullptr;
  unsigned finishingCount = 0, clockReadyCount = 0;
  auto inspect = [&](Operation *op, Value destination) {
    if (destination == finishing) {
      finishingConnect = op;
      ++finishingCount;
    }
    auto field = destination.getDefiningOp<SubfieldOp>();
    if (clockPort && field && field.getInput() == clockPort &&
        field.getFieldName() == "ready") {
      clockReadyConnect = op;
      ++clockReadyCount;
    }
  };
  module.walk([&](ConnectOp op) { inspect(op.getOperation(), op.getDest()); });
  module.walk([&](StrictConnectOp op) {
    inspect(op.getOperation(), op.getDest());
  });
  if (finishingCount > 1 || clockReadyCount > 1) {
    error = "finishing and clock ready each have multiple connects";
    return failure();
  }
  if (finishingConnect && clockReadyConnect &&
      finishingConnect->getBlock() != clockReadyConnect->getBlock()) {
    error = "finishing and clock ready must be in the same FIRRTL block";
    return failure();
  }
  if ((!finishingConnect || (!virtualClock && !clockReadyConnect)) &&
      ((finishingConnect &&
        finishingConnect->getBlock() != module.getBodyBlock()) ||
       (clockReadyConnect &&
        clockReadyConnect->getBlock() != module.getBodyBlock()))) {
    error = "new finishing or clock ready connect needs a module-body peer";
    return failure();
  }

  Operation *insertionPoint = finishingConnect;
  if (clockReadyConnect &&
      (!insertionPoint || clockReadyConnect->isBeforeInBlock(insertionPoint)))
    insertionPoint = clockReadyConnect;
  OpBuilder builder(module.getContext());
  if (insertionPoint)
    builder.setInsertionPoint(insertionPoint);
  else
    builder.setInsertionPointToEnd(module.getBodyBlock());
  Location loc = insertionPoint ? insertionPoint->getLoc() : module.getLoc();
  // Scala's And.reduce begins with the first channel condition.  Preserve
  // that shape so an ordinary FAME model does not gain an extra AND gate.
  // Large hubs otherwise export one enormous inline expression. Materialize
  // electrical nodes periodically so the FIRRTL 1.2 comparison boundary stays
  // readable by SFC without changing the reduction or adding state/gates.
  std::set<std::string> names;
  for (auto port : module.getPorts()) names.insert(port.getName().str());
  module.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      names.insert(name.getValue().str());
  });
  unsigned conditions = 0, group = 0;
  unsigned totalConditions = inputs.size() + outputs.size();
  Value allReady;
  auto addCondition = [&](Value condition) {
    allReady = allReady
                   ? builder.create<AndPrimOp>(loc, allReady, condition)
                         .getResult()
                   : condition;
    if (totalConditions > 64 && ++conditions % 32 == 0 && conditions < totalConditions) {
      std::string name;
      do { name = "allFiredOrFiring_group_" + std::to_string(group++); }
      while (!names.insert(name).second);
      allReady = builder.create<NodeOp>(loc, allReady, name).getResult();
    }
  };
  for (const auto &channel : outputs) {
    Value ready = builder.create<SubfieldOp>(loc, channel.port, "ready");
    Value valid = builder.create<SubfieldOp>(loc, channel.port, "valid");
    Value firing = builder.create<AndPrimOp>(loc, ready, valid).getResult();
    Value satisfied =
        builder.create<OrPrimOp>(loc, channel.fired, firing).getResult();
    addCondition(satisfied);
  }
  for (const auto &channel : inputs) {
    Value valid = builder.create<SubfieldOp>(loc, channel.port, "valid");
    addCondition(valid);
  }
  Value clockValid;
  if (virtualClock)
    clockValid = builder.create<ConstantOp>(
        loc, UIntType::get(module.getContext(), 1, false), APInt(1, 1));
  else
    clockValid = builder.create<SubfieldOp>(loc, clockPort, "valid");
  Value nextCycle = builder.create<AndPrimOp>(loc, allReady, clockValid);
  if (finishingConnect)
    finishingConnect->setOperand(1, nextCycle);
  else
    builder.create<StrictConnectOp>(loc, finishing, nextCycle);
  if (clockReadyConnect)
    clockReadyConnect->setOperand(1, allReady);
  else if (!virtualClock) {
    Value ready = builder.create<SubfieldOp>(loc, clockPort, "ready");
    builder.create<StrictConnectOp>(loc, ready, allReady);
  }
  return success();
}
