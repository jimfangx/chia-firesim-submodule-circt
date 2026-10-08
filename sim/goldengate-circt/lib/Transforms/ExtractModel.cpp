// See LICENSE for license details.
#include "goldengate/ExtractModel.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Support/InstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/DenseSet.h"
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace circt::firrtl;
using namespace mlir;

namespace {
constexpr llvm::StringLiteral modelClass =
    "midas.targetutils.FirrtlFAMEModelAnnotation";

struct ModelInstance {
  FModuleOp parent;
  FModuleOp child;
  InstanceOp instance;
};

std::optional<ModelInstance> resolveModel(CircuitOp circuit,
                                          llvm::StringRef target,
                                          std::string &error) {
  if (!target.consume_front("~")) {
    error = "model target has no circuit";
    return std::nullopt;
  }
  auto [circuitName, local] = target.split('|');
  if (circuitName != circuit.getName() || local.empty()) {
    error = "model target names a different circuit";
    return std::nullopt;
  }
  auto [parentName, instanceAndChild] = local.split('/');
  auto [instanceName, childName] = instanceAndChild.split(':');
  if (parentName.empty() || instanceName.empty() || childName.empty() ||
      childName.contains('/') || childName.contains('>')) {
    error = "model target is not a local instance target";
    return std::nullopt;
  }
  FModuleOp parent, child;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op)) {
      if (module.getName() == parentName)
        parent = module;
      if (module.getName() == childName)
        child = module;
    }
  if (!parent || !child) {
    error = "model target has no internal parent or child module";
    return std::nullopt;
  }
  InstanceOp instance;
  parent.walk([&](InstanceOp candidate) {
    if (candidate.getName() == instanceName &&
        candidate.getModuleName() == childName)
      instance = candidate;
  });
  if (!instance || instance->getBlock() != parent.getBodyBlock()) {
    error = "model instance is missing or conditional";
    return std::nullopt;
  }
  return ModelInstance{parent, child, instance};
}

bool refersToInstance(Attribute attr, llvm::StringRef target) {
  if (auto text = dyn_cast<StringAttr>(attr))
    return text.getValue().starts_with(target);
  if (auto array = dyn_cast<ArrayAttr>(attr)) {
    for (Attribute member : array)
      if (refersToInstance(member, target))
        return true;
  }
  if (auto dict = dyn_cast<DictionaryAttr>(attr)) {
    for (NamedAttribute member : dict)
      if (refersToInstance(member.getValue(), target))
        return true;
  }
  return false;
}

LogicalResult promoteOne(CircuitOp circuit, const ModelInstance &model,
                         SmallVectorImpl<std::string> &peerTargets,
                         std::string &error) {
  auto parent = model.parent;
  auto child = model.child;
  auto instance = model.instance;
  std::string promotedName = instance.getName().str();
  if (parent.getName() == circuit.getName()) {
    error = "cannot promote a model instance from the wrapper top";
    return failure();
  }

  std::vector<InstanceOp> parentUses;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *node = graph.lookup(parent);
    if (!node) {
      error = "model parent is absent from the instance graph";
      return failure();
    }
    for (auto *record : node->uses())
      parentUses.push_back(record->getInstance<InstanceOp>());
  }
  if (parentUses.empty()) {
    error = "model parent has no live instances";
    return failure();
  }
  for (auto use : parentUses)
    if (use->getBlock() != use->getParentOfType<FModuleOp>().getBodyBlock()) {
      error = "conditional parent instances cannot be promoted";
      return failure();
    }

  std::set<std::string> parentNames;
  for (const PortInfo &port : parent.getPorts())
    parentNames.insert(port.getName().str());
  SmallVector<std::pair<unsigned, PortInfo>> added;
  SmallVector<BundleType::BundleElement> fields;
  unsigned oldPortCount = parent.getNumPorts();
  if (!parentNames.insert(promotedName).second) {
    error = "promoted model port name collides: " + promotedName;
    return failure();
  }
  for (unsigned i = 0; i < child.getNumPorts(); ++i) {
    PortInfo childPort = child.getPorts()[i];
    auto baseType = dyn_cast<FIRRTLBaseType>(childPort.type);
    if (!baseType || baseType.containsAnalog()) {
      error = "model promotion requires non-analog FIRRTL ports";
      return failure();
    }
    fields.emplace_back(childPort.name,
                        child.getPortDirection(i) == Direction::In, baseType);
  }
  // Scala's PromoteSubmodule adds one input bundle named after the removed
  // instance.  Its fields retain the child's port names and directions.
  added.push_back({oldPortCount,
                   PortInfo(StringAttr::get(circuit.getContext(), promotedName),
                            BundleType::get(circuit.getContext(), fields),
                            Direction::In)});

  parent.insertPorts(added);
  OpBuilder parentBuilder(&parent.getBodyBlock()->front());
  for (unsigned i = 0; i < child.getNumPorts(); ++i)
    instance.getResult(i).replaceAllUsesWith(parentBuilder.create<SubfieldOp>(
        instance.getLoc(), parent.getBodyBlock()->getArgument(oldPortCount),
        child.getPortName(i)));
  instance.erase();

  for (InstanceOp use : parentUses) {
    auto grandparent = use->getParentOfType<FModuleOp>();
    std::set<std::string> used;
    grandparent.walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("name"))
        used.insert(name.getValue().str());
    });
    std::string base = use.getName().str() + "_" + promotedName;
    std::string peerName = base;
    for (unsigned suffix = 0; used.count(peerName); ++suffix)
      peerName = base + "_" + std::to_string(suffix);

    InstanceOp expanded = use.cloneAndInsertPorts(added);
    for (unsigned i = 0; i < oldPortCount; ++i)
      use.getResult(i).replaceAllUsesWith(expanded.getResult(i));
    use.erase();
    OpBuilder builder(expanded);
    builder.setInsertionPointAfter(expanded);
    InstanceOp peer = builder.create<InstanceOp>(expanded.getLoc(), child,
                                                 peerName);
    peerTargets.push_back("~" + circuit.getName().str() + "|" +
                          grandparent.getName().str() + "/" + peerName +
                          ":" + child.getName().str());
    builder.setInsertionPointAfter(peer);
    for (unsigned i = 0; i < child.getNumPorts(); ++i) {
      Value parentPort = builder.create<SubfieldOp>(
          peer.getLoc(), expanded.getResult(oldPortCount), child.getPortName(i));
      Value childPort = peer.getResult(i);
      // Scala emits a PartialConnect of the two instance bundles. Expand
      // matching aggregate leaves with CIRCT so each nested flip reverses
      // the data flow, including SRAM read/readwrite data fields.
      if (child.getPortDirection(i) == Direction::In)
        emitConnect(builder, peer.getLoc(), childPort, parentPort);
      else
        emitConnect(builder, peer.getLoc(), parentPort, childPort);
    }
  }
  return success();
}
} // namespace

LogicalResult goldengate::extractModels(CircuitOp circuit, unsigned &promoted,
                                        std::string &error) {
  promoted = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "ExtractModel needs retained annotations";
    return failure();
  }
  SmallVector<Attribute> remaining, models;
  llvm::SmallDenseSet<Attribute> selected;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(modelClass))
      remaining.push_back(attr);
    else if (selected.insert(attr).second)
      models.push_back(attr);
  }
  while (!models.empty()) {
    // Promote the outermost labeled model first.  A nested model then becomes
    // a direct child of the wrapper before its own promotion, as in Scala's
    // InstanceGraph module order.
    std::map<std::string, unsigned> depth;
    {
      circt::igraph::InstanceGraph graph(circuit);
      auto *top = graph.lookup(StringAttr::get(circuit.getContext(),
                                              circuit.getName()));
      if (!top) {
        error = "wrapper top is absent from the instance graph";
        return failure();
      }
      std::vector<std::pair<circt::igraph::InstanceGraphNode *, unsigned>>
          queue{{top, 0}};
      for (size_t i = 0; i < queue.size(); ++i) {
        auto [node, distance] = queue[i];
        auto name = cast<FModuleLike>(node->getModule().getOperation())
                        .getModuleName().str();
        if (depth.count(name) && depth[name] <= distance)
          continue;
        depth[name] = distance;
        for (auto *record : *node)
          queue.push_back({record->getTarget(), distance + 1});
      }
    }
    std::optional<unsigned> choice;
    unsigned bestDepth = std::numeric_limits<unsigned>::max();
    for (unsigned i = 0; i < models.size(); ++i) {
      Annotation annotation(models[i]);
      auto target = annotation.getMember<StringAttr>("target");
      if (!target) {
        error = "FAME model annotation has no target";
        return failure();
      }
      llvm::StringRef local = target.getValue().split('|').second;
      std::string parent = local.split('/').first.str();
      // Scala's moduleOrder excludes the circuit main. Already-top models
      // are complete, but other branches still need promotion.
      if (parent == circuit.getName())
        continue;
      auto found = depth.find(parent);
      if (found == depth.end()) {
        error = "FAME model parent is unreachable: " + parent;
        return failure();
      }
      if (found->second < bestDepth) {
        bestDepth = found->second;
        choice = i;
      }
    }
    if (!choice)
      break;
    Attribute attr = models[*choice];
    Annotation annotation(attr);
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) {
      error = "FAME model annotation has no target";
      return failure();
    }
    auto model = resolveModel(circuit, target.getValue(), error);
    if (!model)
      return failure();
    for (Attribute other : remaining)
      if (refersToInstance(other, target.getValue())) {
        error = "another annotation references the promoted model instance";
        return failure();
      }
    SmallVector<std::string> peerTargets;
    if (failed(promoteOne(circuit, *model, peerTargets, error)))
      return failure();
    models.erase(models.begin() + *choice);
    // SFC's RenameMap fans the FAME annotation out to every peer instance.
    // Keep those identities until each branch reaches the circuit main.
    for (const auto &peerTarget : peerTargets) {
      NamedAttrList members(cast<DictionaryAttr>(attr));
      members.set("target", StringAttr::get(circuit.getContext(), peerTarget));
      models.push_back(members.getDictionary(circuit.getContext()));
    }
    ++promoted;
  }
  circuit->setAttr("rawAnnotations",
                   ArrayAttr::get(circuit.getContext(), remaining));
  return success();
}
