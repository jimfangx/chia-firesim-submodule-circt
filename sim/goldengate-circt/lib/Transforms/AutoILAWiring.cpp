// See LICENSE for license details.
#include "goldengate/AutoILAWiring.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>
#include <map>

using namespace mlir;
using namespace circt::firrtl;

namespace {
struct Route {
  goldengate::AutoILAProbe source;
  SmallVector<Operation *> path;
  unsigned port = 0, childPort = 0;
  StringAttr name;
};
struct ModuleRoutes {
  FModuleOp module;
  unsigned oldPorts;
  SmallVector<Route, 0> routes;
  SmallVector<std::pair<unsigned, PortInfo>> added;
};
} // namespace

LogicalResult goldengate::wireAutoILAProbesToTop(
    CircuitOp circuit, SmallVectorImpl<WiredILAProbe> &outputs,
    std::string &error) {
  SmallVector<AutoILAProbe> probes;
  if (failed(analyzeAutoILAProbes(circuit, probes, error))) return failure();
  if (probes.empty()) return success();
  llvm::DenseMap<Value, AutoILAProbe> selected;
  for (auto probe : probes) {
    probe.path.clear();
    selected.try_emplace(probe.value, std::move(probe));
  }
  std::map<Operation *, ModuleRoutes> modules;
  SmallVector<Operation *> order;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  FModuleOp top;
  {
    // All graph queries precede port insertion and instance replacement.
    circt::igraph::InstanceGraph graph(circuit);
    top = cast<FModuleOp>(graph.lookup(
        StringAttr::get(circuit.getContext(), circuit.getName()))->getModule());
    llvm::DenseSet<Operation *> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success();
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in AutoILA top wiring";
        return failure();
      }
      ModuleRoutes info{module, unsigned(module.getNumPorts()), {}, {}};
      auto addLocal = [&](Value value) {
        auto found = selected.find(value);
        if (found != selected.end()) info.routes.push_back({found->second, {}});
      };
      for (auto &op : module.getBodyBlock()->getOperations())
        if (op.getNumResults() == 1) addLocal(op.getResult(0));
      for (auto value : module.getBodyBlock()->getArguments()) addLocal(value);
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto child = modules.find(record->getTarget()->getModule().getOperation());
        if (child == modules.end() || child->second.routes.empty()) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = "AutoILA top wiring requires module-scope FIRRTL instances";
          return failure();
        }
        for (auto route : child->second.routes) {
          route.childPort = route.port;
          route.path.insert(route.path.begin(), instance.getOperation());
          info.routes.push_back(std::move(route));
        }
      }
      circt::Namespace names;
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
      });
      for (auto [index, route] : llvm::enumerate(info.routes)) {
        std::string suggested = "ila_";
        for (auto *instance : route.path)
          suggested += cast<InstanceOp>(instance).getName().str() + "_";
        suggested += route.source.leafName;
        route.port = info.oldPorts + index;
        route.name = StringAttr::get(circuit.getContext(), names.newName(suggested));
        info.added.push_back({info.oldPorts,
            PortInfo(route.name, route.source.value.getType(), Direction::Out)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      order.push_back(key);
      return success();
    };
    // Changing a shared signature must also update uses in unused parents.
    for (auto module : circuit.getOps<FModuleOp>())
      if (failed(plan(graph.lookup(module)))) return failure();
    for (auto *key : order) {
      auto &info = modules.at(key);
      if (info.routes.empty()) continue;
      for (auto *record : graph.lookup(info.module)->uses()) {
        auto instance = record->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!instance || !parent || instance.getNumResults() != info.oldPorts ||
            instance->getBlock() != parent.getBodyBlock()) {
          error = "AutoILA source module has an incompatible instance use";
          return failure();
        }
        uses.push_back({instance, key});
      }
    }
  }
  for (auto *key : order) {
    auto &info = modules.at(key);
    if (!info.added.empty()) info.module.insertPorts(info.added);
  }
  llvm::DenseMap<Operation *, InstanceOp> replacements;
  for (auto [instance, child] : uses) {
    auto &info = modules.at(child);
    auto replacement = instance.cloneAndInsertPorts(info.added);
    for (auto attr : instance->getAttrs())
      if (!replacement->hasAttr(attr.getName()))
        replacement->setAttr(attr.getName(), attr.getValue());
    for (unsigned i = 0; i < info.oldPorts; ++i)
      instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
    replacements[instance.getOperation()] = replacement;
    instance.erase();
  }
  OpBuilder b(circuit.getContext());
  for (auto *key : order) {
    auto &info = modules.at(key);
    b.setInsertionPointToEnd(info.module.getBodyBlock());
    for (auto &route : info.routes) {
      Value driver = route.source.value;
      if (!route.path.empty())
        driver = replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<StrictConnectOp>(info.module.getLoc(),
          info.module.getBodyBlock()->getArgument(route.port), driver);
    }
  }
  SmallVector<WiredILAProbe> result;
  for (auto [index, route] : llvm::enumerate(modules.at(top.getOperation()).routes)) {
    auto probe = route.source;
    probe.index = index;
    probe.suggestedName = "ila_";
    for (auto *old : route.path) {
      auto instance = replacements.lookup(old);
      probe.path.push_back(instance);
      probe.suggestedName += instance.getName().str() + "_";
    }
    probe.suggestedName += probe.leafName;
    result.push_back({std::move(probe), top.getBodyBlock()->getArgument(route.port),
        "~" + circuit.getName().str() + "|" + top.getName().str() + ">" + route.name.getValue().str()});
  }
  outputs.append(result.begin(), result.end());
  return success();
}
