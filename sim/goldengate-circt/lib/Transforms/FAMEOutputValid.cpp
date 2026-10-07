// See LICENSE for license details.
#include "goldengate/FAMEOutputValid.h"
#include "goldengate/FAMEFiredRegister.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/Builders.h"
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct ValidRule {
  Operation *connect;
  Value output;
  Value fired;
  SmallVector<Value> inputPorts;
};

bool isBit(Value value) {
  auto type = dyn_cast<UIntType>(value.getType());
  return type && type.getWidth() == 1;
}
} // namespace

LogicalResult goldengate::rewriteFAMEOutputValids(
    FModuleOp module, llvm::ArrayRef<LocalChannelDependency> dependencies,
    std::string &error) {
  std::map<std::string, unsigned> ports;
  for (unsigned i = 0, n = module.getPorts().size(); i < n; ++i)
    ports.emplace(module.getPortName(i).str(), i);

  auto lookupPort = [&](const std::string &name, Direction direction) -> Value {
    auto found = ports.find(name);
    if (found == ports.end() || module.getPortDirection(found->second) != direction)
      return {};
    return module.getBodyBlock()->getArgument(found->second);
  };
  auto hasValid = [](Value port) {
    auto type = dyn_cast_or_null<BundleType>(port.getType());
    auto index = type ? type.getElementIndex("valid") : std::nullopt;
    // HasModelPort's decoupled valid is always passive. A flipped source
    // valid belongs to the peer, and a flipped sink valid cannot supply the
    // target's combinational dependency. Check both before building any rule.
    return index && !type.getElements()[*index].isFlip &&
           type.getElements()[*index].type ==
                        UIntType::get(port.getContext(), 1, false);
  };

  FAMEFiredRegisterIndex firedRegisters;
  if (failed(firedRegisters.collect(module, error)))
    return failure();
  std::set<std::string> seenOutputs;
  SmallVector<ValidRule> rules;
  for (const auto &dependency : dependencies) {
    if (!dependency.unresolvedPorts.empty() ||
        !dependency.unresolvedCauses.empty()) {
      error = "unresolved dependency for " + dependency.outputChannel;
      return failure();
    }
    if (!seenOutputs.insert(dependency.outputChannel).second) {
      error = "duplicate output channel " + dependency.outputChannel;
      return failure();
    }
    Value output = lookupPort(dependency.outputChannel + "_source",
                              Direction::Out);
    if (!output || !hasValid(output)) {
      error = "missing FAME source valid port for " + dependency.outputChannel;
      return failure();
    }
    Value fired = firedRegisters.lookup(dependency.outputChannel);
    if (!fired || !isBit(fired)) {
      error = "missing one-bit fired register for " + dependency.outputChannel;
      return failure();
    }

    Operation *validConnect = nullptr;
    auto inspectConnect = [&](Operation *op, Value destination) {
      auto field = destination.getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == output &&
          field.getFieldName() == "valid") {
        if (validConnect)
          validConnect = module.getOperation(); // mark duplicate
        else
          validConnect = op;
      }
    };
    module.walk([&](ConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    module.walk([&](StrictConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    if (validConnect == module.getOperation()) {
      error = "source valid has multiple connects for " +
              dependency.outputChannel;
      return failure();
    }

    ValidRule rule{validConnect, output, fired, {}};
    for (const auto &name : dependency.inputChannels) {
      Value input = lookupPort(name + "_sink", Direction::In);
      if (!input || !hasValid(input)) {
        error = "missing FAME sink valid port for " + name;
        return failure();
      }
      rule.inputPorts.push_back(input);
    }
    rules.push_back(std::move(rule));
  }

  // The Scala FAME1OutputChannel rule is AND(required input valids, !fired).
  // Build that equation with FIRRTL operations after every port and register
  // has been checked, so a malformed channel cannot leave a partial rewrite.
  for (const auto &rule : rules) {
    OpBuilder builder(module.getContext());
    if (rule.connect)
      builder.setInsertionPoint(rule.connect);
    else
      builder.setInsertionPointToEnd(module.getBodyBlock());
    Location loc = rule.connect ? rule.connect->getLoc() : module.getLoc();
    Value unfired = builder.create<NotPrimOp>(loc, rule.fired).getResult();
    Value inputValids;
    for (Value port : rule.inputPorts) {
      Value inputValid = builder.create<SubfieldOp>(loc, port, "valid");
      inputValids = inputValids
                        ? builder.create<AndPrimOp>(loc, inputValids, inputValid)
                              .getResult()
                        : inputValid;
    }
    Value valid = inputValids
                      ? builder.create<AndPrimOp>(loc, inputValids, unfired)
                            .getResult()
                      : unfired;
    if (rule.connect) {
      rule.connect->setOperand(1, valid);
    } else {
      Value validField = builder.create<SubfieldOp>(loc, rule.output, "valid");
      builder.create<StrictConnectOp>(loc, validField, valid);
    }
  }
  return success();
}
