// See LICENSE for license details.
#include "goldengate/AutoILAAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/InstanceGraph.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::analyzeAutoILAProbes(
    CircuitOp circuit, llvm::SmallVectorImpl<AutoILAProbe> &probes,
    std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoILA analysis needs retained annotations";
    return failure();
  }
  llvm::DenseMap<Value, AutoILAProbe> selected;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::InternalFpgaDebug)) continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) {
      error = "AutoILA debug annotation lacks a string target";
      return failure();
    }
    std::string spelling = target.getValue().str();
    if (!target.getValue().starts_with("~")) {
      auto circuitAndRest = target.getValue().split('.');
      auto moduleAndRef = circuitAndRest.second.split('.');
      if (circuitAndRest.first != circuit.getName() ||
          moduleAndRef.first.empty() || moduleAndRef.second.empty()) {
        error = "invalid AutoILA ComponentName target: " + spelling;
        return failure();
      }
      spelling = "~" + circuitAndRest.first.str() + "|" +
                 moduleAndRef.first.str() + ">" + moduleAndRef.second.str();
    }
    Value value;
    FModuleOp module;
    bool isPort = false;
    std::string leaf;
    std::string portError;
    if (auto resolved = resolveAnnotationTarget(circuit, spelling, portError)) {
      module = dyn_cast<FModuleOp>(resolved->module.getOperation());
      if (!module || !resolved->port || !resolved->fieldID ||
          *resolved->fieldID != 0) {
        error = "AutoILA needs a ground port in an internal module: " + spelling;
        return failure();
      }
      value = module.getBodyBlock()->getArgument(*resolved->port);
      leaf = module.getPortName(*resolved->port).str();
      isPort = true;
    } else {
      auto *op = resolveInternalAnnotationTarget(circuit, spelling, error);
      if (!op || !isa<NodeOp, WireOp, RegOp, RegResetOp>(op)) {
        error = "AutoILA needs a port, node, wire, or register: " + spelling;
        return failure();
      }
      module = op->getParentOfType<FModuleOp>();
      if (!module || op->getBlock() != module.getBodyBlock()) {
        error = "AutoILA declaration must be at module scope: " + spelling;
        return failure();
      }
      value = op->getResult(0);
      leaf = op->getAttrOfType<StringAttr>("name").getValue().str();
    }
    auto type = dyn_cast<IntType>(value.getType());
    if (!type || type.getWidthOrSentinel() <= 0) {
      error = "AutoILA probe must have a positive known integer width: " + spelling;
      return failure();
    }
    std::string component = circuit.getName().str() + "." +
                            module.getName().str() + "." + leaf;
    selected.try_emplace(value, AutoILAProbe{module, value, isPort,
        unsigned(type.getWidthOrSentinel()), component, leaf, {}, 0, {}});
  }
  if (selected.empty()) return success();

  circt::igraph::InstanceGraph graph(circuit);
  auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
  if (!top || !top->noUses() || !isa<FModuleOp>(top->getModule().getOperation())) {
    error = "AutoILA requires an uninstantiated internal circuit top";
    return failure();
  }
  llvm::DenseMap<Operation *, SmallVector<AutoILAProbe>> routes;
  llvm::DenseSet<Operation *> active;
  std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
      [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
    auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
    if (!module) return success();
    auto *key = module.getOperation();
    if (routes.count(key)) return success();
    if (!active.insert(key).second) {
      error = "recursive module hierarchy in AutoILA analysis";
      return failure();
    }
    SmallVector<AutoILAProbe> local;
    // TopWiring scans the body before it scans the port list.
    for (auto &op : module.getBodyBlock()->getOperations())
      if (op.getNumResults() == 1)
        if (auto found = selected.find(op.getResult(0)); found != selected.end())
          local.push_back(found->second);
    for (auto value : module.getBodyBlock()->getArguments())
      if (auto found = selected.find(value); found != selected.end())
        local.push_back(found->second);
    for (auto *record : *node) {
      if (failed(plan(record->getTarget()))) return failure();
      auto child = routes.find(record->getTarget()->getModule().getOperation());
      if (child == routes.end() || child->second.empty()) continue;
      auto instance = record->getInstance<InstanceOp>();
      if (!instance || instance->getBlock() != module.getBodyBlock()) {
        error = "AutoILA requires module-scope FIRRTL instances";
        return failure();
      }
      for (auto probe : child->second) {
        probe.path.insert(probe.path.begin(), instance);
        local.push_back(std::move(probe));
      }
    }
    active.erase(key);
    routes.try_emplace(key, std::move(local));
    return success();
  };
  if (failed(plan(top))) return failure();
  auto &result = routes.find(top->getModule().getOperation())->second;
  llvm::DenseSet<Value> reachable;
  for (auto [index, probe] : llvm::enumerate(result)) {
    reachable.insert(probe.value);
    probe.index = index;
    probe.suggestedName = "ila_";
    for (auto instance : probe.path)
      probe.suggestedName += instance.getName().str() + "_";
    probe.suggestedName += probe.leafName;
  }
  if (reachable.size() != selected.size()) {
    error = "AutoILA source has no instance path from the circuit top";
    return failure();
  }
  probes.append(result.begin(), result.end());
  return success();
}
