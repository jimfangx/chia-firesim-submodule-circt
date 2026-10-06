// See LICENSE for license details.
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEFiredRegister.h"
#include "mlir/IR/Builders.h"
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct ReadyRule {
  Operation *connect;
  Value sink;
  Value fired;
};

bool isBit(Value value) {
  auto type = dyn_cast<UIntType>(value.getType());
  return type && type.getWidth() == 1;
}
} // namespace

LogicalResult goldengate::rewriteFAMEInputReadies(
    FModuleOp module, llvm::ArrayRef<std::string> inputChannels,
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

  std::set<std::string> seenInputs;
  SmallVector<ReadyRule> rules;
  for (const auto &name : inputChannels) {
    if (!seenInputs.insert(name).second) {
      error = "duplicate input channel " + name;
      return failure();
    }
    auto port = ports.find(name + "_sink");
    if (port == ports.end() ||
        module.getPortDirection(port->second) != Direction::In) {
      error = "missing FAME sink port for " + name;
      return failure();
    }
    Value sink = module.getBodyBlock()->getArgument(port->second);
    auto bundle = dyn_cast<BundleType>(sink.getType());
    auto readyIndex = bundle ? bundle.getElementIndex("ready") : std::nullopt;
    if (!readyIndex || !bundle.getElements()[*readyIndex].isFlip ||
        bundle.getElements()[*readyIndex].type !=
            UIntType::get(sink.getContext(), 1, false)) {
      error = "invalid FAME sink ready field for " + name;
      return failure();
    }
    Value fired = firedRegisters.lookup(name);
    if (!fired || !isBit(fired)) {
      error = "missing one-bit fired register for " + name;
      return failure();
    }

    Operation *readyConnect = nullptr;
    unsigned readyConnectCount = 0;
    auto inspectConnect = [&](Operation *op, Value destination) {
      auto field = destination.getDefiningOp<SubfieldOp>();
      if (field && field.getInput() == sink && field.getFieldName() == "ready") {
        ++readyConnectCount;
        readyConnect = op;
      }
    };
    module.walk([&](ConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    module.walk([&](StrictConnectOp op) {
      inspectConnect(op.getOperation(), op.getDest());
    });
    if (readyConnectCount > 1) {
      error = "sink ready has multiple connects for " + name;
      return failure();
    }
    rules.push_back({readyConnect, sink, fired});
  }

  // Validate the complete channel set before changing the module.
  for (const auto &rule : rules) {
    OpBuilder builder(module.getContext());
    if (rule.connect)
      builder.setInsertionPoint(rule.connect);
    else
      builder.setInsertionPointToEnd(module.getBodyBlock());
    Location loc = rule.connect ? rule.connect->getLoc() : module.getLoc();
    Value notFired = builder.create<NotPrimOp>(loc, rule.fired).getResult();
    Value ready = builder.create<AndPrimOp>(loc, finishing, notFired).getResult();
    if (rule.connect) {
      rule.connect->setOperand(1, ready);
    } else {
      Value readyField = builder.create<SubfieldOp>(loc, rule.sink, "ready");
      builder.create<StrictConnectOp>(loc, readyField, ready);
    }
  }
  return success();
}
