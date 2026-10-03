// See LICENSE for license details.
#include "goldengate/PrintWiring.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::analyzePrintClockSources(
    CircuitOp circuit, ArrayRef<PrintStub> stubs, ArrayRef<WiredPrint> routes,
    SmallVectorImpl<PrintClockSource> &sources, std::string &error) {
  std::map<StringRef, FModuleOp> modules;
  llvm::DenseMap<Value, SmallVector<Value>> drivers;
  for (auto module : circuit.getOps<FModuleOp>()) {
    modules[module.getName()] = module;
    module.walk([&](Operation *op) {
      if (auto connect = dyn_cast<FConnectLike>(op))
        drivers[connect.getDest()].push_back(connect.getSrc());
    });
  }
  auto top = modules[circuit.getName()];
  if (!top) {
    error = "printf clock analysis needs an internal circuit top";
    return failure();
  }
  // A local cone summary contains input Clock port indices, not names or a
  // flattened textual path. Shared modules can have different clock roots in
  // different instances; substitute their input results in the parent cone.
  using Inputs = std::set<unsigned>;
  llvm::DenseMap<Value, Inputs> cache;
  llvm::DenseSet<Value> active;
  std::function<LogicalResult(Value, Inputs &)> cone =
      [&](Value value, Inputs &inputs) -> LogicalResult {
    if (auto found = cache.find(value); found != cache.end()) {
      inputs.insert(found->second.begin(), found->second.end());
      return success();
    }
    if (!active.insert(value).second) {
      error = "combinational cycle in printf clock cone";
      return failure();
    }
    Inputs local;
    if (auto arg = dyn_cast<BlockArgument>(value)) {
      auto module = dyn_cast<FModuleOp>(arg.getOwner()->getParentOp());
      if (!module || arg.getOwner() != module.getBodyBlock()) {
        error = "printf clock cone needs module-scope values";
        return failure();
      }
      if (module.getPortDirection(arg.getArgNumber()) == Direction::In &&
          isa<ClockType>(arg.getType()))
        local.insert(arg.getArgNumber());
    }
    if (auto found = drivers.find(value); found != drivers.end()) {
      for (Value driver : found->second)
        if (failed(cone(driver, local))) return failure();
    } else if (auto result = dyn_cast<OpResult>(value)) {
      auto *op = result.getOwner();
      if (auto instance = dyn_cast<InstanceOp>(op)) {
        if (instance.getPortDirection(result.getResultNumber()) == Direction::Out) {
          auto child = modules.find(instance.getModuleName());
          // External and sequential clock generators have no input Clock root,
          // matching FindClockSources' None result. Never guess a root.
          if (child != modules.end()) {
            Inputs childInputs;
            if (failed(cone(child->second.getBodyBlock()->getArgument(
                                result.getResultNumber()), childInputs)))
              return failure();
            for (unsigned port : childInputs)
              if (failed(cone(instance.getResult(port), local))) return failure();
          }
        }
      } else if (isa<NodeOp>(op) || isExpression(op)) {
        // This boundary requires ground values. Aggregate projections need
        // field-sensitive connectivity, supplied by LowerTypes beforehand.
        if (isa<SubfieldOp, SubindexOp, SubaccessOp>(op)) {
          error = "printf clock cone needs LowerTypes before analysis";
          return failure();
        }
        for (Value operand : op->getOperands())
          if (failed(cone(operand, local))) return failure();
      }
    }
    active.erase(value);
    cache[value] = local;
    inputs.insert(local.begin(), local.end());
    return success();
  };

  SmallVector<PrintClockSource> resolved;
  for (auto [index, route] : llvm::enumerate(routes)) {
    if (route.stubIndex >= stubs.size()) {
      error = "printf clock route has an invalid stub index";
      return failure();
    }
    auto &stub = stubs[route.stubIndex];
    auto module = top;
    for (auto instance : route.instancePath) {
      auto child = modules.find(instance.getModuleName());
      if (instance->getParentOfType<FModuleOp>() != module || child == modules.end()) {
        error = "printf clock route is not an absolute internal instance path";
        return failure();
      }
      module = child->second;
    }
    Value clock = stub.clock;
    auto owner = clock ? (clock.getDefiningOp() ? clock.getDefiningOp()->getParentOfType<FModuleOp>()
        : dyn_cast<FModuleOp>(cast<BlockArgument>(clock).getOwner()->getParentOp())) : FModuleOp();
    if (owner != module || !clock || !isa<ClockType>(clock.getType())) {
      error = "printf local clock does not belong to its absolute instance path";
      return failure();
    }
    unsigned depth = route.instancePath.size();
    while (true) {
      Inputs inputs;
      if (failed(cone(clock, inputs))) return failure();
      if (inputs.size() != 1) {
        error = "printf clock has " + std::to_string(inputs.size()) +
            " input Clock drivers in " + module.getName().str() +
            " for " + route.absoluteSource;
        return failure();
      }
      unsigned port = *inputs.begin();
      if (depth == 0) {
        resolved.push_back({unsigned(index), top.getBodyBlock()->getArgument(port),
            "~" + circuit.getName().str() + "|" + top.getName().str() + ">" +
            top.getPortName(port).str()});
        break;
      }
      auto instance = route.instancePath[--depth];
      clock = instance.getResult(port);
      module = instance->getParentOfType<FModuleOp>();
    }
  }
  sources.append(resolved.begin(), resolved.end());
  return success();
}
