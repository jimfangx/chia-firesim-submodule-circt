// See LICENSE for license details.
#include "goldengate/PrintWiring.h"
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
constexpr llvm::StringLiteral prefix = "synthesizedPrintf_";
struct Route {
  unsigned stubIndex, port;
  SmallVector<Operation *> path;
  StringAttr name;
  // For a descendant source, read this port of path.front().
  unsigned childPort = 0;
};
struct ModuleRoutes {
  FModuleOp module;
  unsigned oldPorts;
  SmallVector<Route> routes;
  SmallVector<std::pair<unsigned, PortInfo>> added;
};
} // namespace

LogicalResult goldengate::wirePrintStubsToTop(
    CircuitOp circuit, ArrayRef<PrintStub> stubs,
    SmallVectorImpl<WiredPrint> &outputs, std::string &error) {
  // All planning uses native instance graph records and operation identities.
  // Destroy the graph before replacing any of its InstanceOps.
  std::map<Operation *, ModuleRoutes> modules;
  SmallVector<Operation *> order;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *topNode = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!topNode || !topNode->noUses() ||
        !(top = dyn_cast<FModuleOp>(topNode->getModule().getOperation()))) {
      error = "printf top wiring needs an uninstantiated internal circuit top";
      return failure();
    }
    llvm::DenseMap<Operation *, SmallVector<unsigned>> local;
    llvm::DenseSet<Operation *> selected;
    for (auto [i, stub] : llvm::enumerate(stubs)) {
      auto bundle = stub.bundle;
      auto module = bundle ? bundle->getParentOfType<FModuleOp>() : FModuleOp();
      auto type = bundle ? dyn_cast<BundleType>(bundle.getResult().getType()) : BundleType();
      if (!module || module->getParentOp() != circuit.getOperation() ||
          bundle->getBlock() != module.getBodyBlock() || !type || !type.isPassive() ||
          !selected.insert(bundle.getOperation()).second) {
        error = "printf top wiring needs distinct module-scope passive bundle sources";
        return failure();
      }
      local[module.getOperation()].push_back(i);
    }
    llvm::DenseSet<Operation *> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success(); // Extmodules have no local printf sources.
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in printf top wiring";
        return failure();
      }
      ModuleRoutes info{module, unsigned(module.getNumPorts()), {}, {}};
      for (unsigned index : local[key])
        info.routes.push_back({index, 0, {}, {}, 0});
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto child = modules.find(record->getTarget()->getModule().getOperation());
        if (child == modules.end() || child->second.routes.empty()) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = "printf top wiring requires module-scope FIRRTL instances";
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
      for (auto [i, route] : llvm::enumerate(info.routes)) {
        std::string suggested = prefix.str();
        for (auto *instance : route.path)
          suggested += cast<InstanceOp>(instance).getName().str() + "_";
        auto bundle = stubs[route.stubIndex].bundle;
        suggested += bundle.getName().str();
        route.port = info.oldPorts + i;
        route.name = StringAttr::get(circuit.getContext(), names.newName(suggested));
        info.added.push_back({info.oldPorts,
            PortInfo(route.name, bundle.getResult().getType(), Direction::Out)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      order.push_back(key);
      return success();
    };
    // Include unused parents so their instances also receive the expanded
    // child signature. Their existing circuitry and metadata remain valid.
    for (auto module : circuit.getOps<FModuleOp>())
      if (failed(plan(graph.lookup(module)))) return failure();
    llvm::DenseSet<unsigned> reachable;
    for (auto &route : modules.at(top.getOperation()).routes)
      reachable.insert(route.stubIndex);
    if (reachable.size() != stubs.size()) {
      error = "printf bundle source has no instance path from the circuit top";
      return failure();
    }
    for (auto &[key, info] : modules) {
      if (info.routes.empty()) continue;
      for (auto *record : graph.lookup(info.module)->uses()) {
        auto instance = record->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!instance || !parent || instance.getNumResults() != info.oldPorts ||
            instance->getBlock() != parent.getBodyBlock()) {
          error = "printf source module has an incompatible instance use";
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
    // CIRCT rebuilds the signature and known instance attributes. Retain any
    // additional metadata without overwriting the expanded port attributes.
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
      auto bundle = stubs[route.stubIndex].bundle;
      Value driver = bundle.getResult();
      if (!route.path.empty())
        driver = replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<StrictConnectOp>(bundle.getLoc(),
          info.module.getBodyBlock()->getArgument(route.port), driver);
    }
  }
  SmallVector<WiredPrint> result;
  for (auto &route : modules.at(top.getOperation()).routes) {
    auto bundle = stubs[route.stubIndex].bundle;
    std::string absolute = "~" + circuit.getName().str() + "|" + top.getName().str();
    SmallVector<Operation *> path;
    for (auto *old : route.path) {
      auto instance = replacements.lookup(old);
      path.push_back(instance);
      absolute += "/" + instance.getName().str() + ":" + instance.getModuleName().str();
    }
    absolute += ">" + bundle.getName().str();
    result.push_back({route.stubIndex, std::move(path),
        top.getBodyBlock()->getArgument(route.port), absolute,
        "~" + circuit.getName().str() + "|" + top.getName().str() + ">" + route.name.getValue().str()});
  }
  outputs.append(result.begin(), result.end());
  return success();
}
