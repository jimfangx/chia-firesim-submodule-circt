// See LICENSE for license details.
#include "goldengate/PromotePassthroughConnections.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <optional>

using namespace circt::firrtl;
using namespace mlir;

namespace {
class PassthroughTracer {
public:
  explicit PassthroughTracer(CircuitOp circuit) {
    for (Operation &op : circuit.getBodyBlock()->getOperations())
      if (auto module = dyn_cast<FModuleOp>(&op))
        modules.emplace(module.getName().str(), module);
  }

  std::optional<Value> trace(FModuleOp module, Value value) {
    llvm::DenseSet<Value> active;
    return trace(module, value, active);
  }

private:
  struct Drivers {
    llvm::DenseMap<Value, Value> source;
    llvm::DenseSet<Value> ambiguous;
  };

  Drivers &getDrivers(FModuleOp module) {
    auto [entry, inserted] = drivers.try_emplace(module.getOperation());
    if (inserted) {
      auto add = [&](Value dest, Value source) {
        if (!entry->second.source.try_emplace(dest, source).second)
          entry->second.ambiguous.insert(dest);
      };
      module.walk([&](ConnectOp op) {
        if (op->getBlock() == module.getBodyBlock())
          add(op.getDest(), op.getSrc());
        else
          entry->second.ambiguous.insert(op.getDest());
      });
      module.walk([&](StrictConnectOp op) {
        if (op->getBlock() == module.getBodyBlock())
          add(op.getDest(), op.getSrc());
        else
          entry->second.ambiguous.insert(op.getDest());
      });
    }
    return entry->second;
  }

  std::optional<Value> trace(FModuleOp module, Value value,
                             llvm::DenseSet<Value> &active) {
    if (!active.insert(value).second)
      return std::nullopt;
    auto finish = [&](std::optional<Value> result) {
      active.erase(value);
      return result;
    };

    if (auto argument = dyn_cast<BlockArgument>(value)) {
      if (argument.getOwner() != module.getBodyBlock())
        return finish(std::nullopt);
      if (module.getPortDirection(argument.getArgNumber()) == Direction::In)
        return finish(value);
    }
    if (auto instance = value.getDefiningOp<InstanceOp>()) {
      unsigned port = cast<OpResult>(value).getResultNumber();
      auto child = modules.find(instance.getModuleName().str());
      if (child == modules.end())
        return finish(value);
      if (child->second.getPortDirection(port) == Direction::Out) {
        auto childSource =
            trace(child->second,
                  child->second.getBodyBlock()->getArgument(port), active);
        auto input = childSource ? dyn_cast<BlockArgument>(*childSource)
                                 : BlockArgument();
        if (!input || input.getOwner() != child->second.getBodyBlock() ||
            child->second.getPortDirection(input.getArgNumber()) !=
                Direction::In)
          return finish(value);
        return finish(trace(module, instance.getResult(input.getArgNumber()),
                            active));
      }
      // An instance input is a sink in its parent.  Continue through its
      // parent-side connect below, just as for a wire.
    }
    if (auto node = value.getDefiningOp<NodeOp>())
      return finish(trace(module, node->getOperand(0), active));

    // Only exact whole-value connects are wire identities.  A FIRRTL
    // primitive, aggregate field, or multiply driven value is not one.
    auto &moduleDrivers = getDrivers(module);
    if (moduleDrivers.ambiguous.contains(value))
      return finish(std::nullopt);
    auto driver = moduleDrivers.source.find(value);
    if (driver == moduleDrivers.source.end())
      return finish(std::nullopt);
    return finish(trace(module, driver->second, active));
  }

  std::map<std::string, FModuleOp> modules;
  llvm::DenseMap<Operation *, Drivers> drivers;
};
} // namespace

LogicalResult goldengate::promotePassthroughConnections(
    CircuitOp circuit, unsigned &promoted, std::string &error) {
  promoted = 0;
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  if (!top) {
    error = "missing FAME wrapper top module";
    return failure();
  }
  PassthroughTracer tracer(circuit);
  auto rewrite = [&](Operation *op, Value dest, Value source) {
    if (isa<ClockType>(dest.getType()))
      return;
    bool isSink = false;
    if (auto arg = dyn_cast<BlockArgument>(dest))
      isSink = arg.getOwner() == top.getBodyBlock() &&
               top.getPortDirection(arg.getArgNumber()) == Direction::Out;
    if (auto instance = dest.getDefiningOp<InstanceOp>()) {
      auto port = cast<OpResult>(dest).getResultNumber();
      isSink = instance->getParentOfType<FModuleOp>() == top &&
               instance.getPortDirection(port) == Direction::In;
    }
    if (!isSink)
      return;
    auto root = tracer.trace(top, source);
    if (!root || *root == source || root->getType() != source.getType())
      return;
    op->setOperand(1, *root);
    ++promoted;
  };
  top.walk([&](ConnectOp op) {
    if (op->getBlock() == top.getBodyBlock())
      rewrite(op.getOperation(), op.getDest(), op.getSrc());
  });
  top.walk([&](StrictConnectOp op) {
    if (op->getBlock() == top.getBodyBlock())
      rewrite(op.getOperation(), op.getDest(), op.getSrc());
  });
  return success();
}
