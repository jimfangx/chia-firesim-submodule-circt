// See LICENSE for license details.
#include "goldengate/FAMEFiredState.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct FiredRule {
  Operation *connect;
  Value fired;
  Value port;
  Value enable;
};

bool isBit(Value value) {
  auto type = dyn_cast<UIntType>(value.getType());
  return type && type.getWidth() == 1;
}
} // namespace

LogicalResult goldengate::ensureFAMEFiredRegisters(
    FModuleOp module, llvm::ArrayRef<FAMEFiredChannel> channels,
    std::string &error) {
  Value hostClock, hostReset;
  std::set<std::string> names;
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i) {
    auto name = module.getPortName(i);
    names.insert(name.str());
    if (module.getPortDirection(i) != Direction::In)
      continue;
    if (name == "hostClock")
      hostClock = module.getBodyBlock()->getArgument(i);
    if (name == "hostReset")
      hostReset = module.getBodyBlock()->getArgument(i);
  }
  if (!hostClock || !hostReset || !isa<ClockType>(hostClock.getType()) ||
      !isBit(hostReset)) {
    error = "missing host clock or reset input";
    return failure();
  }

  WireOp finishing;
  std::map<std::string, RegResetOp> registers;
  module.walk([&](WireOp op) {
    names.insert(op.getName().str());
    if (op.getName() == "targetCycleFinishing")
      finishing = op;
  });
  module.walk([&](RegOp op) { names.insert(op.getName().str()); });
  module.walk([&](RegResetOp op) {
    names.insert(op.getName().str());
    registers[op.getName().str()] = op;
  });
  if (!finishing || !isBit(finishing.getResult())) {
    error = "missing one-bit targetCycleFinishing wire";
    return failure();
  }

  std::set<std::string> seenChannels;
  SmallVector<std::pair<std::string, bool>> toCreate;
  for (const auto &channel : channels) {
    if (!seenChannels.insert(channel.name).second) {
      error = "duplicate data channel " + channel.name;
      return failure();
    }
    auto portName = channel.name +
                    (channel.isInput ? "_sink" : "_source");
    bool foundPort = false;
    for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i)
      if (module.getPortName(i) == portName &&
          module.getPortDirection(i) ==
              (channel.isInput ? Direction::In : Direction::Out))
        foundPort = true;
    if (!foundPort) {
      error = "missing decoupled channel port for " + channel.name;
      return failure();
    }

    auto fired = registers.find(channel.name + "_fired_0");
    if (fired == registers.end())
      fired = registers.find(channel.name + "_fired");
    if (fired != registers.end()) {
      auto resetValue = fired->second.getResetValue().getDefiningOp<ConstantOp>();
      unsigned expectedReset = channel.isInput ? 1 : 0;
      if (!isBit(fired->second.getResult()) ||
          fired->second.getClockVal() != hostClock ||
          fired->second.getResetSignal() != hostReset || !resetValue ||
          resetValue.getValue().getZExtValue() != expectedReset) {
        error = "invalid fired register clock or reset for " + channel.name;
        return failure();
      }
      continue;
    }
    std::string newName = channel.name + "_fired_0";
    if (names.count(newName) || names.count(channel.name + "_fired")) {
      error = "fired register name is already in use for " + channel.name;
      return failure();
    }
    names.insert(newName);
    toCreate.push_back({std::move(newName), channel.isInput});
  }

  OpBuilder declarations(finishing);
  declarations.setInsertionPointAfter(finishing);
  OpBuilder connects(module.getContext());
  connects.setInsertionPointToEnd(module.getBodyBlock());
  auto bitType = UIntType::get(module.getContext(), 1, false);
  for (const auto &[name, isInput] : toCreate) {
    Location loc = finishing.getLoc();
    Value resetValue = declarations.create<ConstantOp>(
        loc, bitType, APInt(1, isInput ? 1 : 0)).getResult();
    Value fired = declarations.create<RegResetOp>(
        loc, bitType, hostClock, hostReset, resetValue, name).getResult();
    connects.create<StrictConnectOp>(loc, fired, fired);
  }
  return success();
}

LogicalResult goldengate::rewriteFAMEFiredStates(
    FModuleOp module, llvm::ArrayRef<FAMEFiredChannel> channels,
    std::string &error) {
  Value finishing;
  module.walk([&](WireOp op) {
    if (op.getName() == "targetCycleFinishing")
      finishing = op.getResult();
  });
  if (!finishing || !isBit(finishing)) {
    error = "missing one-bit targetCycleFinishing wire";
    return failure();
  }

  std::map<std::string, Value> firedRegisters;
  module.walk([&](RegResetOp op) {
    firedRegisters[op.getName().str()] = op.getResult();
  });
  std::map<std::string, unsigned> ports;
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i)
    ports.emplace(module.getPortName(i).str(), i);
  auto hostClockPort = ports.find("hostClock");
  auto hostResetPort = ports.find("hostReset");
  if (hostClockPort == ports.end() || hostResetPort == ports.end()) {
    error = "missing host clock or reset port";
    return failure();
  }
  Value hostClock = module.getBodyBlock()->getArgument(hostClockPort->second);
  Value hostReset = module.getBodyBlock()->getArgument(hostResetPort->second);
  if (module.getPortDirection(hostClockPort->second) != Direction::In ||
      module.getPortDirection(hostResetPort->second) != Direction::In ||
      !isa<ClockType>(hostClock.getType()) || !isBit(hostReset)) {
    error = "invalid host clock or reset port";
    return failure();
  }

  std::set<std::string> seenChannels;
  SmallVector<FiredRule> rules;
  for (const auto &channel : channels) {
    if (!seenChannels.insert(channel.name).second) {
      error = "duplicate data channel " + channel.name;
      return failure();
    }
    if (!channel.clockDomainEnable || !isBit(channel.clockDomainEnable)) {
      error = "missing one-bit clock enable for " + channel.name;
      return failure();
    }
    auto port = ports.find(channel.name +
                           (channel.isInput ? "_sink" : "_source"));
    Direction direction = channel.isInput ? Direction::In : Direction::Out;
    if (port == ports.end() ||
        module.getPortDirection(port->second) != direction) {
      error = "missing decoupled channel port for " + channel.name;
      return failure();
    }
    Value bundlePort = module.getBodyBlock()->getArgument(port->second);
    auto bundle = dyn_cast<BundleType>(bundlePort.getType());
    auto readyIndex = bundle ? bundle.getElementIndex("ready") : std::nullopt;
    auto validIndex = bundle ? bundle.getElementIndex("valid") : std::nullopt;
    auto bitType = UIntType::get(bundlePort.getContext(), 1, false);
    if (!readyIndex || !validIndex ||
        bundle.getElements()[*readyIndex].type != bitType ||
        bundle.getElements()[*validIndex].type != bitType ||
        !bundle.getElements()[*readyIndex].isFlip ||
        bundle.getElements()[*validIndex].isFlip) {
      error = "invalid FAME ready/valid fields for " + channel.name;
      return failure();
    }
    auto fired = firedRegisters.find(channel.name + "_fired_0");
    if (fired == firedRegisters.end())
      fired = firedRegisters.find(channel.name + "_fired");
    if (fired == firedRegisters.end() || !isBit(fired->second)) {
      error = "missing one-bit resettable fired register for " + channel.name;
      return failure();
    }
    auto registerOp = fired->second.getDefiningOp<RegResetOp>();
    if (registerOp.getClockVal() != hostClock ||
        registerOp.getResetSignal() != hostReset) {
      error = "fired register must use host clock and reset for " +
              channel.name;
      return failure();
    }

    Operation *firedConnect = nullptr;
    unsigned firedConnectCount = 0;
    auto inspectConnect = [&](Operation *op, Value destination) {
      if (destination != fired->second)
        return;
      ++firedConnectCount;
      if (firedConnectCount == 1)
        firedConnect = op;
    };
    module.walk([&](ConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    module.walk([&](StrictConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    if (firedConnectCount != 1) {
      error = "fired register needs exactly one connect for " + channel.name;
      return failure();
    }
    rules.push_back({firedConnect, fired->second, bundlePort,
                     channel.clockDomainEnable});
  }

  // The Scala rule is fired <= finishing ? !clockDomainEnable :
  //                             fired | (ready & valid). Construct the rule
  // using FIRRTL operations after validating every channel.
  for (const auto &rule : rules) {
    OpBuilder builder(rule.connect);
    Location loc = rule.connect->getLoc();
    Value ready = builder.create<SubfieldOp>(loc, rule.port, "ready");
    Value valid = builder.create<SubfieldOp>(loc, rule.port, "valid");
    Value firing = builder.create<AndPrimOp>(loc, ready, valid).getResult();
    Value firedOrFiring =
        builder.create<OrPrimOp>(loc, rule.fired, firing).getResult();
    Value notEnabled =
        builder.create<NotPrimOp>(loc, rule.enable).getResult();
    Value nextState = builder.create<MuxPrimOp>(
        loc, finishing, notEnabled, firedOrFiring).getResult();
    rule.connect->setOperand(1, nextState);
  }
  return success();
}
