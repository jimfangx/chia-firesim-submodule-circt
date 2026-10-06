// See LICENSE for license details.
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFiredRegister.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/APSInt.h"
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
  circt::Namespace names;
  std::set<std::string> reserved;
  auto reserve = [&](StringRef name) {
    if (!name.empty() && reserved.insert(name.str()).second)
      names.newName(name);
  };
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i) {
    auto name = module.getPortName(i);
    reserve(name);
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
  module.walk([&](WireOp op) {
    if (op.getName() == "targetCycleFinishing")
      finishing = op;
  });
  // Include every named declaration, including nodes, memories and instances.
  module.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name");
        name && !name.getValue().empty())
      reserve(name.getValue());
  });
  if (!finishing || !isBit(finishing.getResult())) {
    error = "missing one-bit targetCycleFinishing wire";
    return failure();
  }

  std::set<std::string> seenChannels;
  FAMEFiredRegisterIndex registers;
  if (failed(registers.collect(module, error)))
    return failure();
  struct Declaration {
    std::string name, channel;
    bool resetFired;
  };
  SmallVector<Declaration> toCreate;
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

    // Only explicitly identified host state may be reused. A target register
    // with the historical fired name is still target state.
    if (Value fired = registers.lookup(channel.name, false)) {
      auto reg = fired.getDefiningOp<RegResetOp>();
      auto resetValue = reg.getResetValue().getDefiningOp<ConstantOp>();
      unsigned expectedReset = channel.isInput && channel.hasClockDomain;
      if (!isBit(fired) ||
          reg.getClockVal() != hostClock ||
          reg.getResetSignal() != hostReset || !resetValue ||
          resetValue.getValue().getZExtValue() != expectedReset) {
        error = "invalid fired register clock or reset for " + channel.name;
        return failure();
      }
      continue;
    }
    // SFC genMetadata reserves a suggestion, then hostFlagReg uniques it again.
    std::string suggestion = names.newName(channel.name + "_fired").str();
    std::string newName = names.newName(suggestion).str();
    toCreate.push_back({std::move(newName), channel.name,
                        channel.isInput && channel.hasClockDomain});
  }

  OpBuilder declarations(finishing);
  declarations.setInsertionPointAfter(finishing);
  OpBuilder connects(module.getContext());
  connects.setInsertionPointToEnd(module.getBodyBlock());
  auto bitType = UIntType::get(module.getContext(), 1, false);
  for (const auto &[name, channel, resetFired] : toCreate) {
    Location loc = finishing.getLoc();
    Value resetValue = declarations.create<ConstantOp>(
        loc, bitType, APInt(1, resetFired ? 1 : 0)).getResult();
    auto fired = declarations.create<RegResetOp>(
        loc, bitType, hostClock, hostReset, resetValue, name);
    fired->setAttr(fameFiredChannelAttr, declarations.getStringAttr(channel));
    connects.create<StrictConnectOp>(loc, fired.getResult(), fired.getResult());
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

  FAMEFiredRegisterIndex firedRegisters;
  if (failed(firedRegisters.collect(module, error)))
    return failure();
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
    if (!channel.hasClockDomain) {
      auto enable = channel.clockDomainEnable.getDefiningOp<ConstantOp>();
      if (!enable || !enable.getValue().isOne()) {
        error = "virtual-clock channel requires constant-one enable for " +
                channel.name;
        return failure();
      }
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
    Value fired = firedRegisters.lookup(channel.name);
    if (!fired || !isBit(fired) || !fired.getDefiningOp<RegResetOp>()) {
      error = "missing one-bit resettable fired register for " + channel.name;
      return failure();
    }
    auto registerOp = fired.getDefiningOp<RegResetOp>();
    if (registerOp.getClockVal() != hostClock ||
        registerOp.getResetSignal() != hostReset) {
      error = "fired register must use host clock and reset for " +
              channel.name;
      return failure();
    }
    auto resetValue = registerOp.getResetValue().getDefiningOp<ConstantOp>();
    if (!resetValue || resetValue.getValue().getZExtValue() !=
                           unsigned(channel.isInput && channel.hasClockDomain)) {
      error = "fired register has incorrect SFC reset value for " + channel.name;
      return failure();
    }

    Operation *firedConnect = nullptr;
    unsigned firedConnectCount = 0;
    auto inspectConnect = [&](Operation *op, Value destination) {
      if (destination != fired)
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
    rules.push_back({firedConnect, fired, bundlePort,
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
