// See LICENSE for license details.
#include "goldengate/GlobalResetWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/DenseMap.h"
#include <functional>
#include <map>

using namespace mlir;
using namespace circt::firrtl;

namespace {
bool isSource(Annotation annotation) {
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSource) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSource);
}
bool isSink(Annotation annotation) {
  return annotation.isClass(goldengate::AnnotationClasses::GlobalResetSink) ||
         annotation.isClass(goldengate::AnnotationClasses::PublicGlobalResetSink);
}
struct Reference { FModuleOp module; Value value; };
struct ModuleRoute {
  FModuleOp module;
  unsigned oldPorts;
  bool needed = false;
  SmallVector<std::pair<unsigned, PortInfo>> added;
  SmallVector<std::pair<Operation *, Operation *>> children;
};
std::optional<Reference> resolve(CircuitOp circuit, StringRef target, bool sink,
                               std::string &error) {
  std::string portError;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target, portError)) {
    auto module = dyn_cast<FModuleOp>(port->module.getOperation());
    if (!module || !port->port || port->fieldID.value_or(0) != 0 ||
        (sink && module.getPortDirection(*port->port) != Direction::Out)) {
      error = "global reset needs a lowered ground reference; port sinks must be outputs: " + target.str();
      return std::nullopt;
    }
    return Reference{module, module.getBodyBlock()->getArgument(*port->port)};
  }
  auto *op = goldengate::resolveInternalAnnotationTarget(circuit, target, error);
  if (!op || !isa<WireOp, NodeOp, RegOp, RegResetOp>(op) ||
      (sink && !isa<WireOp>(op))) {
    error = "global reset needs a local lowered declaration; internal sinks must be wires: " + target.str();
    return std::nullopt;
  }
  auto module = op->getParentOfType<FModuleOp>();
  if (!module || op->getBlock() != module.getBodyBlock()) {
    error = "global reset declaration must be at module scope: " + target.str();
    return std::nullopt;
  }
  return Reference{module, op->getResult(0)};
}
} // namespace

LogicalResult goldengate::wireGlobalReset(CircuitOp circuit, unsigned &wired,
                                          std::string &error) {
  wired = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "global reset wiring needs retained annotations"; return failure(); }
  SmallVector<Attribute> retained;
  SmallVector<StringAttr> sources, sinks;
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!isSource(annotation) && !isSink(annotation)) {
      retained.push_back(attr);
      continue;
    }
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) { error = "global reset annotation has no target"; return failure(); }
    (isSource(annotation) ? sources : sinks).push_back(target);
  }
  // Scala's parameterized wiring transform requires at most one source even
  // when sinks are absent. It does not resolve targets when either side is absent.
  if (sources.size() > 1) { error = "received multiple global reset source annotations"; return failure(); }
  if (sources.empty() || sinks.empty()) {
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
    return success();
  }
  auto source = resolve(circuit, sources.front().getValue(), false, error);
  if (!source) return failure();
  auto isBool = [](Value value) {
    auto type = dyn_cast<UIntType>(value.getType());
    return type && type.getWidth() == 1;
  };
  if (!isBool(source->value)) { error = "global reset source must be UInt<1>"; return failure(); }
  SmallVector<Reference> destinations;
  llvm::DenseSet<Operation *> sinkModules;
  bool crossModule = false;
  llvm::DenseSet<Value> seen;
  SmallVector<Operation *> oldDrivers;
  llvm::DenseSet<Operation *> seenDrivers;
  for (auto target : sinks) {
    auto sink = resolve(circuit, target.getValue(), true, error);
    if (!sink) return failure();
    if (!isBool(sink->value) ||
        sink->value == source->value) {
      error = "global reset wiring currently needs distinct UInt<1> source and sinks";
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
        error = "global reset wiring requires ExpandWhens before replacing sink drivers";
        return failure();
      }
      if (seenDrivers.insert(op).second) oldDrivers.push_back(op);
    }
  }
  std::map<Operation *, ModuleRoute> modules;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  if (crossModule) {
    // Plan through native instance identities. No source-instance ownership
    // ambiguity exists in this subset: the source is in the circuit top.
    // Destroy the graph before cloning instances or changing port signatures.
    circt::igraph::InstanceGraph graph(circuit);
    auto *top = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!top || !top->noUses() || top->getModule().getOperation() != source->module) {
      error = "cross-module global reset wiring requires a circuit-top source";
      return failure();
    }
    llvm::DenseSet<Operation *> active;
    auto sourceName = sources.front().getValue().split('>').second;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success();
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in global reset wiring";
        return failure();
      }
      ModuleRoute info{module, unsigned(module.getNumPorts()), sinkModules.contains(key), {}, {}};
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto *childKey = record->getTarget()->getModule().getOperation();
        auto child = modules.find(childKey);
        if (child == modules.end() || !child->second.needed) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = "global reset routing requires module-scope FIRRTL instances";
          return failure();
        }
        info.needed = true;
        info.children.push_back({instance.getOperation(), childKey});
      }
      if (info.needed && module != source->module) {
        circt::Namespace names;
        for (auto name : module.getPortNamesAttr())
          names.newName(cast<StringAttr>(name).getValue());
        module.walk([&](Operation *op) {
          if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
        });
        // Scala Wiring uses its annotation key at sink modules and the
        // source reference name at intermediate modules.
        auto name = names.newName(sinkModules.contains(key) ?
                                  StringRef("InternalGlobalResetCondition") : sourceName);
        info.added.push_back({info.oldPorts,
            PortInfo(StringAttr::get(circuit.getContext(), name), source->value.getType(), Direction::In)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      return success();
    };
    if (failed(plan(top))) return failure();
    for (auto *key : sinkModules)
      if (!modules.count(key)) {
        error = "global reset sink has no instance path from the circuit top";
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
          error = "global reset route has an unreachable or incompatible instance use";
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
  auto resetValue = [&](FModuleOp module) -> Value {
    if (module == source->module) return source->value;
    return module.getBodyBlock()->getArgument(modules.at(module.getOperation()).oldPorts);
  };
  // Replace flat placeholder drivers; this is equivalent to Scala's appended
  // last-connect drivers. Retain the source and its passthrough blocker logic.
  for (auto *op : oldDrivers) op->erase();
  OpBuilder b(circuit.getContext());
  for (auto sink : destinations) {
    b.setInsertionPointToEnd(sink.module.getBodyBlock());
    b.create<StrictConnectOp>(sink.module.getLoc(), sink.value, resetValue(sink.module));
  }
  for (auto &[key, info] : modules) {
    b.setInsertionPointToEnd(info.module.getBodyBlock());
    for (auto [oldInstance, child] : info.children)
      b.create<StrictConnectOp>(info.module.getLoc(),
          replacements.lookup(oldInstance).getResult(modules.at(child).oldPorts), resetValue(info.module));
  }
  wired = destinations.size();
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
  return success();
}
