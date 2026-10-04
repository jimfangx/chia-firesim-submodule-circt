// See LICENSE for license details.
#include "goldengate/GlobalResetWiring.h"
#include "goldengate/HostClockWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <iterator>
#include <functional>
#include <map>

using namespace mlir;
using namespace circt::firrtl;

namespace {
bool isSource(Annotation annotation, bool hostClock) {
  if (hostClock) return annotation.isClass(goldengate::AnnotationClasses::HostClockSource);
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSource) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSource);
}
bool isSink(Annotation annotation, bool hostClock) {
  if (hostClock) return annotation.isClass(goldengate::AnnotationClasses::HostClockSink);
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSink) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSink);
}
struct Reference { FModuleOp module; Value value; };
struct ModuleRoute {
  FModuleOp module;
  unsigned oldPorts;
  bool needed = false;
  bool exportsSource = false;
  SmallVector<std::pair<unsigned, PortInfo>> added;
  SmallVector<std::pair<Operation *, Operation *>> children;
};
std::optional<Reference> resolve(CircuitOp circuit, StringRef target, bool sink,
                               const std::string &label, std::string &error) {
  std::string portError;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target, portError)) {
    auto module = dyn_cast<FModuleOp>(port->module.getOperation());
    if (!module || !port->port || port->fieldID.value_or(0) != 0 ||
        (sink && module.getPortDirection(*port->port) != Direction::Out)) {
      error = label + " needs a lowered ground reference; port sinks must be outputs: " + target.str();
      return std::nullopt;
    }
    return Reference{module, module.getBodyBlock()->getArgument(*port->port)};
  }
  auto *op = goldengate::resolveInternalAnnotationTarget(circuit, target, error);
  if (!op || !isa<WireOp, NodeOp, RegOp, RegResetOp>(op) ||
      (sink && !isa<WireOp>(op))) {
    error = label + " needs a local lowered declaration; internal sinks must be wires: " + target.str();
    return std::nullopt;
  }
  auto module = op->getParentOfType<FModuleOp>();
  if (!module || op->getBlock() != module.getBodyBlock()) {
    error = label + " declaration must be at module scope: " + target.str();
    return std::nullopt;
  }
  return Reference{module, op->getResult(0)};
}

LogicalResult wireSignal(CircuitOp circuit, unsigned &wired,
                         std::string &error, bool hostClock, bool retainSource) {
  const std::string label = hostClock ? "host clock" : "global reset";
  const std::string typeName = hostClock ? "Clock" : "UInt<1>";
  wired = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = label + " wiring needs retained annotations"; return failure(); }
  SmallVector<Attribute> retained, sourceAnnotations;
  SmallVector<StringAttr> sources, sinks;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!isSource(annotation, hostClock) && !isSink(annotation, hostClock)) {
      retained.push_back(attr);
      continue;
    }
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) { error = label + " annotation has no target"; return failure(); }
    (isSource(annotation, hostClock) ? sources : sinks).push_back(target);
    if (retainSource && isSource(annotation, hostClock)) sourceAnnotations.push_back(attr);
  }
  // Scala's parameterized wiring transform requires at most one source even
  // when sinks are absent. It does not resolve targets when either side is absent.
  if (sources.size() > 1) { error = "received multiple " + label + " source annotations"; return failure(); }
  if (hostClock && sources.empty()) {
    error = "host clock wiring requires exactly one source annotation";
    return failure();
  }
  auto consume = [&] {
    // HostClockWiring.apply prepends its original source for later invocations;
    // the final compiler execute path consumes both classes.
    sourceAnnotations.append(retained.begin(), retained.end());
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), sourceAnnotations));
  };
  if (sources.empty() || sinks.empty()) {
    consume();
    return success();
  }
  auto source = resolve(circuit, sources.front().getValue(), false, label, error);
  if (!source) return failure();
  auto isSignal = [&](Value value) {
    if (hostClock) return isa<ClockType>(value.getType());
    auto type = dyn_cast<UIntType>(value.getType());
    return type && type.getWidth() == 1;
  };
  if (!isSignal(source->value)) { error = label + " source must be " + typeName; return failure(); }
  SmallVector<Reference> destinations;
  llvm::DenseSet<Operation *> sinkModules;
  bool crossModule = false;
  llvm::DenseSet<Value> seen;
  SmallVector<Operation *> oldDrivers;
  llvm::DenseSet<Operation *> seenDrivers;
  for (auto target : sinks) {
    auto sink = resolve(circuit, target.getValue(), true, label, error);
    if (!sink) return failure();
    if (!isSignal(sink->value) ||
        sink->value == source->value) {
      error = label + " wiring currently needs distinct " + typeName + " source and sinks";
      return failure();
    }
    if (!seen.insert(sink->value).second) continue;
    destinations.push_back(*sink);
    sinkModules.insert(sink->module.getOperation());
    crossModule |= sink->module != source->module;
    for (auto &use : sink->value.getUses()) {
      auto *op = use.getOwner();
      bool driver = false;
      if (auto connect = dyn_cast<StrictConnectOp>(op)) driver = connect.getDest() == sink->value;
      if (auto connect = dyn_cast<ConnectOp>(op)) driver = connect.getDest() == sink->value;
      if (!driver) continue;
      if (op->getBlock() != sink->module.getBodyBlock()) {
        error = label + " wiring requires ExpandWhens before replacing sink drivers";
        return failure();
      }
      if (seenDrivers.insert(op).second) oldDrivers.push_back(op);
    }
  }
  std::map<Operation *, ModuleRoute> modules;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  if (crossModule) {
    // Plan through native instance identities. A nested source must have one
    // absolute instance path, including a unique use of every ancestor.
    // Destroy the graph before cloning instances or changing port signatures.
    circt::igraph::InstanceGraph graph(circuit);
    auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!top || !top->noUses()) {
      error = label + " wiring requires an uninstantiated circuit top";
      return failure();
    }
    SmallVector<circt::igraph::InstanceGraphNode *> sourcePath;
    llvm::DenseSet<Operation *> sourceAncestors;
    auto *sourceNode = graph.lookup(source->module);
    while (sourceNode != top) {
      auto *key = sourceNode->getModule().getOperation();
      if (!sourceAncestors.insert(key).second ||
          std::distance(sourceNode->uses().begin(), sourceNode->uses().end()) != 1) {
        error = "cross-module " + label + " wiring requires a unique source instance path";
        return failure();
      }
      sourcePath.push_back(sourceNode);
      auto *use = *sourceNode->uses().begin();
      auto instance = use->getInstance<InstanceOp>();
      auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
      if (!parent || instance->getBlock() != parent.getBodyBlock()) {
        error = label + " source path requires module-scope FIRRTL instances";
        return failure();
      }
      sourceNode = graph.lookup(parent);
    }
    sourceAncestors.insert(top->getModule().getOperation());
    sourcePath.push_back(top);
    std::reverse(sourcePath.begin(), sourcePath.end());

    // Pathless sinks denote every instance. Find their common ancestor with
    // the unique source path before deciding which ports need to change.
    unsigned lca = sourcePath.size() - 1;
    SmallVector<circt::igraph::InstanceGraphNode *> path;
    llvm::DenseSet<Operation *> active, reachedSinks;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> findLCA =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto *key = node->getModule().getOperation();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in " + label + " wiring";
        return failure();
      }
      path.push_back(node);
      if (sinkModules.contains(key)) {
        reachedSinks.insert(key);
        unsigned common = 0;
        while (common < path.size() && common < sourcePath.size() &&
               path[common] == sourcePath[common]) ++common;
        lca = std::min(lca, common - 1);
      }
      for (auto *record : *node)
        if (failed(findLCA(record->getTarget()))) return failure();
      path.pop_back();
      active.erase(key);
      return success();
    };
    if (failed(findLCA(top))) return failure();
    for (auto *key : sinkModules)
      if (!reachedSinks.contains(key)) {
        error = label + " sink has no instance path from the circuit top";
        return failure();
      }
    auto *routeRoot = sourcePath[lca];
    auto sourceName = sources.front().getValue().split('>').second;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success();
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in " + label + " wiring";
        return failure();
      }
      ModuleRoute info{module, unsigned(module.getNumPorts()),
                       sinkModules.contains(key) || sourceAncestors.contains(key),
                       sourceAncestors.contains(key), {}, {}};
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto *childKey = record->getTarget()->getModule().getOperation();
        auto child = modules.find(childKey);
        if (child == modules.end() || !child->second.needed) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = label + " routing requires module-scope FIRRTL instances";
          return failure();
        }
        info.needed = true;
        info.children.push_back({instance.getOperation(), childKey});
      }
      if (info.needed && node != routeRoot) {
        circt::Namespace names;
        for (auto name : module.getPortNamesAttr())
          names.newName(cast<StringAttr>(name).getValue());
        module.walk([&](Operation *op) {
          if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
        });
        // Scala Wiring uses its annotation key at sink modules and the
        // source reference name at intermediate modules.
        auto name = names.newName(sinkModules.contains(key) ?
                                  StringRef(hostClock ? "HostClockSource" : "InternalGlobalResetCondition") : sourceName);
        info.added.push_back({info.oldPorts,
            PortInfo(StringAttr::get(circuit.getContext(), name), source->value.getType(),
                     info.exportsSource ? Direction::Out : Direction::In)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      return success();
    };
    if (failed(plan(routeRoot))) return failure();
    for (auto *key : sinkModules)
      if (!modules.count(key)) {
        error = label + " sink has no instance path from the circuit top";
        return failure();
      }
    for (auto &[key, info] : modules) {
      if (info.added.empty()) continue;
      for (auto *record : graph.lookup(info.module)->uses()) {
        auto instance = record->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        auto parentInfo = parent ? modules.find(parent.getOperation()) : modules.end();
        if (!instance || parentInfo == modules.end() || !parentInfo->second.needed ||
            instance.getNumResults() != info.oldPorts || instance->getBlock() != parent.getBodyBlock()) {
          error = label + " route has an unreachable or incompatible instance use";
          return failure();
        }
        uses.push_back({instance, key});
      }
    }
  }

  for (auto &[key, info] : modules)
    if (!info.added.empty()) info.module.insertPorts(info.added);
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
  // Use the native source/child SSA value at the LCA instead of introducing
  // Scala Wiring's alias wire. This preserves the same signal identity.
  auto signalValue = [&](FModuleOp module) -> Value {
    if (module == source->module) return source->value;
    auto &info = modules.at(module.getOperation());
    if (info.exportsSource)
      for (auto [oldInstance, child] : info.children)
        if (modules.at(child).exportsSource)
          return replacements.lookup(oldInstance).getResult(modules.at(child).oldPorts);
    return module.getBodyBlock()->getArgument(info.oldPorts);
  };
  // Replace flat placeholder drivers; this is equivalent to Scala's appended
  // last-connect drivers. Retain the source and its passthrough blocker logic.
  for (auto *op : oldDrivers) op->erase();
  OpBuilder b(circuit.getContext());
  for (auto sink : destinations) {
    b.setInsertionPointToEnd(sink.module.getBodyBlock());
    b.create<StrictConnectOp>(sink.module.getLoc(), sink.value, signalValue(sink.module));
  }
  for (auto &[key, info] : modules) {
    b.setInsertionPointToEnd(info.module.getBodyBlock());
    if (info.exportsSource && !info.added.empty())
      b.create<StrictConnectOp>(info.module.getLoc(),
          info.module.getBodyBlock()->getArgument(info.oldPorts), signalValue(info.module));
    for (auto [oldInstance, child] : info.children)
      if (!modules.at(child).exportsSource)
        b.create<StrictConnectOp>(info.module.getLoc(),
            replacements.lookup(oldInstance).getResult(modules.at(child).oldPorts), signalValue(info.module));
  }
  wired = destinations.size();
  consume();
  return success();
}

} // namespace

LogicalResult goldengate::wireGlobalReset(CircuitOp circuit, unsigned &wired,
                                          std::string &error) {
  return wireSignal(circuit, wired, error, false, false);
}

LogicalResult goldengate::wireHostClock(CircuitOp circuit, unsigned &wired,
                                       std::string &error, bool retainSource) {
  return wireSignal(circuit, wired, error, true, retainSource);
}
