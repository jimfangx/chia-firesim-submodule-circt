// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <optional>

// Read-only identity tracing through whole-value connects, nodes, wires and
// module boundaries. Ambiguous/conditional drivers and cycles remain unknown.
// Opaque logic is never treated as an identity of one of its operands.
namespace goldengate {
class PassthroughSourceAnalysis {
public:
  explicit PassthroughSourceAnalysis(circt::firrtl::CircuitOp circuit,
                                     bool preserveModelOutputs = true)
      : preserveModelOutputs(preserveModelOutputs) {
    for (mlir::Operation &op : circuit.getBodyBlock()->getOperations())
      if (auto module = mlir::dyn_cast<circt::firrtl::FModuleOp>(&op))
        modules.emplace(module.getName().str(), module);
  }

  std::optional<mlir::Value> trace(circt::firrtl::FModuleOp module, mlir::Value value) {
    llvm::DenseSet<mlir::Value> active;
    return trace(module, value, active);
  }

private:
  struct Drivers {
    llvm::DenseMap<mlir::Value, mlir::Value> source;
    llvm::DenseSet<mlir::Value> ambiguous;
  };

  Drivers &getDrivers(circt::firrtl::FModuleOp module) {
    auto [entry, inserted] = drivers.try_emplace(module.getOperation());
    if (inserted) {
      auto add = [&](mlir::Value dest, mlir::Value source) {
        auto [driver, inserted] = entry->second.source.try_emplace(dest, source);
        // Clock identity queries may see one identical clock assignment per
        // memory accessor. They all establish the same physical connection.
        // Keep model-channel promotion's original multiple-driver boundary.
        if (!inserted && (preserveModelOutputs || driver->second != source))
          entry->second.ambiguous.insert(dest);
      };
      module.walk([&](circt::firrtl::ConnectOp op) {
        if (op->getBlock() == module.getBodyBlock())
          add(op.getDest(), op.getSrc());
        else
          entry->second.ambiguous.insert(op.getDest());
      });
      module.walk([&](circt::firrtl::StrictConnectOp op) {
        if (op->getBlock() == module.getBodyBlock())
          add(op.getDest(), op.getSrc());
        else
          entry->second.ambiguous.insert(op.getDest());
      });
    }
    return entry->second;
  }

  std::optional<mlir::Value> trace(circt::firrtl::FModuleOp module, mlir::Value value,
                             llvm::DenseSet<mlir::Value> &active,
                             mlir::Value outputRoot = {}) {
    if (!active.insert(value).second)
      return std::nullopt;
    auto finish = [&](std::optional<mlir::Value> result) {
      active.erase(value);
      return result;
    };

    if (auto argument = mlir::dyn_cast<mlir::BlockArgument>(value)) {
      if (argument.getOwner() != module.getBodyBlock())
        return finish(std::nullopt);
      if (module.getPortDirection(argument.getArgNumber()) == circt::firrtl::Direction::In)
        return finish(value);
      // Retain the last output on an identity path. If its driver is opaque
      // (a primitive, register or memory), that output is the model source.
      // An output-to-output alias must not acquire a separate FAME channel.
      if (preserveModelOutputs)
        outputRoot = value;
    }
    if (auto instance = value.getDefiningOp<circt::firrtl::InstanceOp>()) {
      unsigned port = mlir::cast<mlir::OpResult>(value).getResultNumber();
      auto child = modules.find(instance.getModuleName().str());
      if (child == modules.end())
        return finish(outputRoot ? outputRoot : value);
      if (child->second.getPortDirection(port) == circt::firrtl::Direction::Out) {
        auto childSource =
            trace(child->second,
                  child->second.getBodyBlock()->getArgument(port), active);
        auto portRoot = childSource ? mlir::dyn_cast<mlir::BlockArgument>(*childSource)
                                    : mlir::BlockArgument();
        if (!portRoot || portRoot.getOwner() != child->second.getBodyBlock())
          return finish(outputRoot ? outputRoot : value);
        if (child->second.getPortDirection(portRoot.getArgNumber()) ==
            circt::firrtl::Direction::Out)
          return finish(outputRoot ? outputRoot
                                   : instance.getResult(portRoot.getArgNumber()));
        return finish(trace(module, instance.getResult(portRoot.getArgNumber()),
                            active, outputRoot));
      }
      // An instance input is a sink in its parent.  Continue through its
      // parent-side connect below, just as for a wire.
    }
    if (auto node = value.getDefiningOp<circt::firrtl::NodeOp>())
      return finish(trace(module, node->getOperand(0), active, outputRoot));
    // A register's next-state connect is not an identity edge through the
    // register. Only wires and instance inputs may follow their drivers.
    if (auto *definition = value.getDefiningOp();
        definition && !mlir::isa<circt::firrtl::WireOp, circt::firrtl::InstanceOp>(definition))
      return finish(outputRoot ? std::optional<mlir::Value>(outputRoot)
                               : preserveModelOutputs ? std::nullopt
                                                      : std::optional<mlir::Value>(value));

    // Only exact whole-value connects are wire identities.  A FIRRTL
    // primitive, aggregate field, or multiply driven value is not one.
    auto &moduleDrivers = getDrivers(module);
    if (moduleDrivers.ambiguous.contains(value))
      return finish(std::nullopt);
    auto driver = moduleDrivers.source.find(value);
    if (driver == moduleDrivers.source.end())
      return finish(outputRoot ? std::optional<mlir::Value>(outputRoot) : std::nullopt);
    return finish(trace(module, driver->second, active, outputRoot));
  }

  // Model-channel promotion must stop at exported model sources. Clock
  // identity queries may use an opaque local SSA value as an identity, but
  // never equate opaque values belonging to separate hierarchy instances.
  bool preserveModelOutputs;
  std::map<std::string, circt::firrtl::FModuleOp> modules;
  llvm::DenseMap<mlir::Operation *, Drivers> drivers;
};
} // namespace
