// See LICENSE for license details.
#include "goldengate/TriggerWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "circt/Support/Namespace.h"
#include "circt/Support/InstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringMap.h"
#include <functional>

using namespace circt::firrtl;
using namespace mlir;
using A = goldengate::AnnotationClasses;

namespace {
bool source(Annotation a) {
  return a.isClass(A::TriggerSource) || a.isClass(A::InternalTriggerSource);
}
bool sink(Annotation a) {
  return a.isClass(A::TriggerSink) || a.isClass(A::InternalTriggerSink);
}
bool boolean(Value v) {
  auto type = v ? dyn_cast<UIntType>(v.getType()) : UIntType();
  return type && type.getWidth() == 1;
}
// Annotations may select aggregate leaves before LowerTypes. Resolve
// identity without building projections: malformed targets must be atomic.
struct LocalField { FModuleOp module; circt::FieldRef field; };
LocalField resolveLocalField(CircuitOp circuit, StringAttr target,
                             std::string &error) {
  if (!target) { error = "trigger annotation has a missing field reference"; return {}; }
  std::string portError;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target.getValue(), portError)) {
    auto module = dyn_cast<FModuleOp>(port->module.getOperation());
    if (module && port->port)
      return {module, circt::FieldRef(module.getBodyBlock()->getArgument(*port->port),
                                    port->fieldID.value_or(0))};
  }
  if (auto local = goldengate::resolveInternalFieldTarget(circuit, target.getValue(), error)) {
    auto module = dyn_cast<FModuleOp>(local->module.getOperation());
    if (module && local->declaration->getBlock() == module.getBodyBlock())
      return {module, circt::FieldRef(local->declaration->getResult(0), local->fieldID)};
  }
  error = "trigger reference needs a local field in an internal module: " + target.getValue().str();
  return {};
}
circt::FieldRef resolveField(CircuitOp circuit, FModuleOp module, StringAttr target,
                            std::string &error) {
  auto local = resolveLocalField(circuit, target, error);
  if (!local.field) return {};
  if (local.module == module) return local.field;
  error = "trigger clock/reset reference must belong to its accounting module: " + target.getValue().str();
  return {};
}
// Return the selected leaf type and its accumulated bundle orientation.
std::pair<Type, bool> leaf(circt::FieldRef field) {
  Type type = field.getValue().getType();
  unsigned id = field.getFieldID();
  bool flipped = false;
  while (id) {
    if (auto bundle = dyn_cast<BundleType>(type)) {
      if (id > bundle.getMaxFieldID()) return {};
      auto [index, childID] = bundle.getIndexAndSubfieldID(id);
      auto element = bundle.getElement(index);
      flipped ^= element.isFlip;
      type = element.type; id = childID;
    } else if (auto vector = dyn_cast<FVectorType>(type)) {
      if (id > vector.getMaxFieldID()) return {};
      auto childID = vector.getIndexAndSubfieldID(id).second;
      type = vector.getElementType(); id = childID;
    } else return {};
  }
  return {type, flipped};
}
bool boolean(circt::FieldRef field) {
  auto type = field ? dyn_cast_or_null<UIntType>(leaf(field).first) : UIntType();
  return type && type.getWidth() == 1;
}
struct Source { circt::FieldRef event, reset, clock; bool credit; std::string name; };
// BridgeTopWiring groups source events by the upstream input Clock port.
// Prove electrical aliases using transparent FIRRTL operations only. A cone
// with one input clock is insufficient: a mux or gate may change its edges.
class LocalClockAliases {
  using Field = circt::FieldRef;
public:
  LocalClockAliases(CircuitOp circuit, FModuleOp top) : top(top) {
    for (auto module : circuit.getOps<FModuleOp>()) {
      modules[module.getName()] = module;
      module.walk([&](Operation *op) {
        Value dest, src;
        if (auto connect = dyn_cast<ConnectOp>(op)) {
          dest = connect.getDest(); src = connect.getSrc();
        } else if (auto connect = dyn_cast<StrictConnectOp>(op)) {
          dest = connect.getDest(); src = connect.getSrc();
        }
        if (dest)
          indexConnect(getFieldRefFromValue(dest), getFieldRefFromValue(src),
                       dest.getType(), src.getType(),
                       op->getBlock() == module.getBodyBlock());
      });
    }
  }
  Field root(Field field) {
    if (!field) return {};
    llvm::DenseSet<Field> active;
    // Retain the selected input leaf. Projections have different SSA identities
    // and must not merge distinct clocks belonging to the same aggregate port.
    return trace(top, field, active);
  }
  Field root(FModuleOp module, Field field, ArrayRef<InstanceOp> path) {
    llvm::DenseSet<Field> active;
    field = trace(module, field, active);
    for (auto instance : path) {
      auto arg = field ? dyn_cast<BlockArgument>(field.getValue()) : BlockArgument();
      if (!arg || arg.getOwner() != module.getBodyBlock() ||
          arg.getArgNumber() >= instance.getNumResults()) return {};
      module = instance->getParentOfType<FModuleOp>();
      field = trace(module, Field(instance.getResult(arg.getArgNumber()), field.getFieldID()), active);
    }
    return module == top ? field : Field();
  }
private:
  // Index clock leaves without creating Subfield/Subindex operations. Two
  // projections of the same field have different SSA values but one driver.
  // Whole aggregate connects contribute drivers to each selected clock leaf;
  // flipped fields reverse the connection direction.
  void indexConnect(Field dest, Field src, Type destType, Type srcType,
                    bool unconditional, bool flipped = false) {
    if (isa<ClockType>(destType) && isa<ClockType>(srcType)) {
      drivers[flipped ? src : dest].push_back(
          {flipped ? dest : src, unconditional});
    } else if (auto bundle = dyn_cast<BundleType>(destType)) {
      auto source = dyn_cast<BundleType>(srcType);
      if (!source) return;
      for (unsigned i = 0; i < bundle.getNumElements(); ++i) {
        auto element = bundle.getElement(i);
        auto j = source.getElementIndex(element.name);
        if (!j) continue;
        indexConnect(dest.getSubField(bundle.getFieldID(i)),
                     src.getSubField(source.getFieldID(*j)), element.type,
                     source.getElementType(*j), unconditional,
                     flipped ^ element.isFlip);
      }
    } else if (auto vector = dyn_cast<FVectorType>(destType)) {
      auto source = dyn_cast<FVectorType>(srcType);
      if (!source || source.getNumElements() != vector.getNumElements()) return;
      for (unsigned i = 0; i < vector.getNumElements(); ++i)
        indexConnect(dest.getSubField(vector.getFieldID(i)),
                     src.getSubField(source.getFieldID(i)), vector.getElementType(),
                     source.getElementType(), unconditional, flipped);
    }
  }
  Field trace(FModuleOp module, Field field, llvm::DenseSet<Field> &active) {
    if (!field) return {};
    auto [type, flipped] = leaf(field);
    if (!isa_and_nonnull<ClockType>(type) || !active.insert(field).second) return {};
    Value value = field.getValue();
    auto followDriver = [&]() -> Field {
      auto &assigned = drivers[field];
      if (assigned.size() != 1 || !assigned.front().second) return {};
      return trace(module, assigned.front().first, active);
    };
    Field result;
    if (auto arg = dyn_cast<BlockArgument>(value)) {
      if (arg.getOwner() == module.getBodyBlock()) {
        if ((module.getPortDirection(arg.getArgNumber()) == Direction::In) ^ flipped) {
          if (drivers[field].empty()) result = field;
        } else result = followDriver();
      }
    } else if (auto *op = value.getDefiningOp();
               op && op->getBlock() == module.getBodyBlock()) {
      if (auto node = dyn_cast<NodeOp>(op)) {
        if (drivers[field].empty())
          result = trace(module,
              getFieldRefFromValue(node.getInput()).getSubField(field.getFieldID()), active);
      } else if (isa<WireOp>(op)) {
        result = followDriver();
      } else if (auto instance = dyn_cast<InstanceOp>(op)) {
        auto child = modules.find(instance.getModuleName());
        auto port = cast<OpResult>(value).getResultNumber();
        if (child != modules.end() && port < child->second.getNumPorts()) {
          if ((child->second.getPortDirection(port) == Direction::In) ^ flipped) {
            result = followDriver();
          } else if (drivers[field].empty()) {
            // Resolve a child output to that child's input, then return through
            // this specific instance. Shared module definitions must not merge
            // clocks connected to different parent inputs.
            auto childRoot = trace(child->second,
                Field(child->second.getBodyBlock()->getArgument(port), field.getFieldID()), active);
            auto argument = childRoot ? dyn_cast<BlockArgument>(childRoot.getValue())
                                      : BlockArgument();
            if (argument && argument.getArgNumber() < instance.getNumResults())
              result = trace(module, Field(instance.getResult(argument.getArgNumber()),
                                           childRoot.getFieldID()), active);
          }
        }
      }
    }
    active.erase(field);
    return result;
  }
  FModuleOp top;
  llvm::StringMap<FModuleOp> modules;
  llvm::DenseMap<Field, SmallVector<std::pair<Field, bool>>> drivers;
};
} // namespace

LogicalResult goldengate::wireTriggers(CircuitOp circuit, unsigned &consumed,
                                      std::string &error) {
  consumed = 0;
  error.clear();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "TriggerWiring needs retained annotations"; return failure(); }
  SmallVector<Attribute> retained;
  SmallVector<Annotation> sources, sinks;
  unsigned credits = 0, debits = 0;
  for (Attribute attr : raw) {
    Annotation a(attr);
    if (sink(a)) { sinks.push_back(a); continue; }
    if (!source(a)) { retained.push_back(attr); continue; }
    auto kind = a.getMember<BoolAttr>("sourceType");
    if (!kind) { error = "trigger source has no sourceType"; return failure(); }
    kind.getValue() ? ++credits : ++debits;
    sources.push_back(a);
  }
  if ((credits == 0) != (debits == 0)) {
    error = "trigger credits and debits must both be present"; return failure();
  }
  // Scala skips target resolution when either side is absent, then consumes
  // trigger annotations. This includes sinks with no sources.
  if (sources.empty() || sinks.empty()) {
    consumed = sources.size();
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
    return success();
  }
  FModuleOp top;
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == circuit.getName()) top = module;
  if (!top) { error = "trigger hardware needs a circuit top"; return failure(); }
  StringAttr baseTarget;
  for (auto attr : raw) {
    Annotation a(attr);
    auto info = a.getMember<DictionaryAttr>("channelInfo");
    if (!a.isClass(A::ChannelConnection) || !info ||
        info.getAs<StringAttr>("class") != A::TargetClockChannel) continue;
    auto targets = a.getMember<ArrayAttr>("sinks");
    if (targets && !targets.empty()) {
      baseTarget = dyn_cast<StringAttr>(targets[0]);
      break;
    }
  }
  auto baseField = resolveField(circuit, top, baseTarget, error);
  if (!baseField) return failure();
  LocalClockAliases aliases(circuit, top);
  // Local accounting uses BridgeTopWiring's root; the synchronizers and
  // global counters retain the annotated base clock, as in Scala.
  auto clockRoot = aliases.root(baseField);
  if (!clockRoot) {
    error = "trigger base clock needs an unconditional alias of a top input Clock port";
    return failure();
  }
  SmallVector<Source> events;
  llvm::MapVector<circt::FieldRef, std::pair<unsigned, unsigned>> domainSources;
  circt::igraph::InstanceGraph graph(circuit);
  llvm::MapVector<Operation *, SmallVector<InstanceOp>> sourceParentInstances;
  llvm::MapVector<Operation *, SmallVector<InstanceOp>> sinkParentInstances;
  llvm::MapVector<Operation *, SmallVector<unsigned>> childEvents;
  llvm::DenseMap<Operation *, unsigned> depths;
  llvm::DenseSet<circt::FieldRef> creditTargets, debitTargets;
  SmallVector<std::pair<NodeOp, circt::FieldRef>> nodes;
  llvm::MapVector<Operation *, bool> sinkModules;
  llvm::DenseMap<Operation *, unsigned> sinkAnnotations;
  // A pathless source contributes once per absolute instance, including uses
  // in different parent definitions. Validate complete paths, but rewrite
  // each definition and direct use only once, from descendants upward.
  auto routeSourceToTop = [&](FModuleOp module,
                              SmallVector<SmallVector<InstanceOp>> &paths,
                              SmallVector<FModuleOp> &pathModules) -> LogicalResult {
    llvm::DenseSet<Operation *> seenModules, activeModules;
    SmallVector<InstanceOp> path;
    SmallVector<FModuleOp> ancestors;
    std::function<LogicalResult(FModuleOp)> visit = [&](FModuleOp current) -> LogicalResult {
      if (current == top) {
        paths.push_back(path);
        // Unequal routes through a shared definition must use the longest
        // depth, so all its exports exist before any ancestor is rewritten.
        for (auto [index, ancestor] : llvm::enumerate(ancestors))
          depths[ancestor] = std::max(depths.lookup(ancestor), unsigned(path.size() - index));
        return success();
      }
      if (!activeModules.insert(current).second) {
        error = "trigger hierarchy contains a cycle"; return failure();
      }
      if (seenModules.insert(current).second) pathModules.push_back(current);
      ancestors.push_back(current);
      bool hasUse = false;
      for (auto *use : graph.lookup(current)->uses()) {
        auto instance = use->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!parent || instance->getBlock() != parent.getBodyBlock() ||
            instance.getNumResults() != current.getNumPorts()) {
          error = "trigger source needs unconditional instances"; return failure();
        }
        hasUse = true;
        auto &instances = sourceParentInstances[current];
        if (!llvm::is_contained(instances, instance)) instances.push_back(instance);
        path.push_back(instance);
        if (failed(visit(parent))) return failure();
        path.pop_back();
      }
      if (!hasUse) {
        error = "trigger source needs an instance route to top"; return failure();
      }
      ancestors.pop_back();
      activeModules.erase(current);
      return success();
    };
    return visit(module);
  };
  // A pathless sink applies to every instance, including instances of its
  // ancestors. Enumerate complete paths for clock preflight, but record each
  // module definition and direct instance only once for input rewiring.
  auto routeSinkToTop = [&](FModuleOp module,
                            SmallVector<SmallVector<InstanceOp>> &paths,
                            SmallVector<FModuleOp> &pathModules) -> LogicalResult {
    llvm::DenseSet<Operation *> seenModules, activeModules;
    SmallVector<InstanceOp> path;
    SmallVector<FModuleOp> ancestors;
    std::function<LogicalResult(FModuleOp)> visit = [&](FModuleOp current) -> LogicalResult {
      if (current == top) {
        paths.push_back(path);
        // Keep the longest route across every sink. A definition may occur at
        // different depths; all parent enables must exist before its uses.
        for (auto [index, ancestor] : llvm::enumerate(ancestors))
          depths[ancestor] = std::max(depths.lookup(ancestor), unsigned(path.size() - index));
        return success();
      }
      if (!activeModules.insert(current).second) {
        error = "trigger sink hierarchy contains a cycle"; return failure();
      }
      if (seenModules.insert(current).second) pathModules.push_back(current);
      ancestors.push_back(current);
      bool hasUse = false;
      for (auto *use : graph.lookup(current)->uses()) {
        auto instance = use->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!parent || instance->getBlock() != parent.getBodyBlock() ||
            instance.getNumResults() != current.getNumPorts()) {
          error = "trigger sink fanout needs unconditional instances"; return failure();
        }
        hasUse = true;
        auto &instances = sinkParentInstances[current];
        if (!llvm::is_contained(instances, instance)) instances.push_back(instance);
        path.push_back(instance);
        if (failed(visit(parent))) return failure();
        path.pop_back();
      }
      if (!hasUse) {
        error = "trigger sink needs an instance route to top"; return failure();
      }
      ancestors.pop_back();
      activeModules.erase(current);
      return success();
    };
    return visit(module);
  };
  for (auto a : sources) {
    auto target = a.getMember<StringAttr>("target");
    auto local = resolveLocalField(circuit, target, error);
    auto event = local.field;
    if (!event) return failure();
    SmallVector<SmallVector<InstanceOp>> paths;
    SmallVector<FModuleOp> pathModules;
    if (failed(routeSourceToTop(local.module, paths, pathModules))) return failure();
    auto eventClock = resolveField(circuit, local.module, a.getMember<StringAttr>("clock"), error);
    if (!eventClock) return failure();
    if (!boolean(event)) {
      error = "trigger sources must be UInt<1>"; return failure();
    }
    // BridgeTopWiring groups exports by their upstream input Clock leaf.
    // A descendant definition can use a secondary root without exporting its
    // clock: prove that every absolute instance resolves to that same leaf.
    // Distinct roots per instance need per-export grouping in a later step.
    auto eventRoot = aliases.root(local.module, eventClock, paths.front());
    if (!eventRoot) {
      error = "trigger source clock needs an unconditional top input Clock alias";
      return failure();
    }
    for (auto &path : paths)
      if (!(aliases.root(local.module, eventClock, path) == eventRoot)) {
        error = "trigger source instance clock paths must resolve to one top input Clock leaf"; return failure();
      }
    bool credit = a.getMember<BoolAttr>("sourceType").getValue();
    auto &counts = domainSources[eventRoot];
    ++(credit ? counts.first : counts.second);
    if (!(credit ? creditTargets : debitTargets).insert(event).second) {
      error = "trigger hardware currently needs distinct source targets per sourceType";
      return failure();
    }
    circt::FieldRef reset;
    if (auto resetTarget = a.getMember<StringAttr>("reset")) {
      reset = resolveField(circuit, local.module, resetTarget, error);
      if (!reset) return failure();
      if (!boolean(reset)) { error = "trigger reset must be UInt<1>"; return failure(); }
    } else if (a.getDict().get("reset")) {
      error = "trigger reset must be a reference when present"; return failure();
    }
    auto name = getFieldName(event, /*nameSafe=*/true).first;
    for (auto module : pathModules) childEvents[module].push_back(events.size());
    events.push_back({event, reset, eventRoot, credit, name});
  }
  // Scala's per-domain DensePrefixSum requires both event lists to be nonempty.
  // Reject an incomplete domain before materializing any hardware.
  for (auto &[domain, counts] : domainSources)
    if (!counts.first || !counts.second) {
      error = "trigger accounting needs credits and debits in each source clock domain";
      return failure();
    }
  DominanceInfo dominance(circuit);
  // Scala onModuleSink constructs a map by node name: the last annotation
  // wins. Select it before resolving clocks, then visit declarations in IR
  // order as onStmtSink does. Annotation order must not assign synchronizer
  // identities (or clock operands) to different nodes.
  for (auto [index, a] : llvm::enumerate(sinks)) {
    auto local = resolveLocalField(circuit, a.getMember<StringAttr>("target"), error);
    if (!local.field) return failure();
    Value value = local.field.getValue();
    auto node = value.getDefiningOp<NodeOp>();
    if (!node || local.field.getFieldID() || !boolean(value)) {
      error = "trigger sinks must be ground local UInt<1> nodes"; return failure();
    }
    sinkAnnotations[node] = index;
  }
  SmallVector<NodeOp> orderedSinks;
  circuit.walk([&](NodeOp node) {
    if (sinkAnnotations.count(node)) orderedSinks.push_back(node);
  });
  for (auto node : orderedSinks) {
    auto module = node->getParentOfType<FModuleOp>();
    auto a = sinks[sinkAnnotations.lookup(node)];
    SmallVector<SmallVector<InstanceOp>> paths;
    SmallVector<FModuleOp> pathModules;
    if (failed(routeSinkToTop(module, paths, pathModules))) return failure();
    auto sinkClock = resolveField(circuit, module, a.getMember<StringAttr>("clock"), error);
    if (!sinkClock) return failure();
    // Scala synchronizes each sink on its annotated local clock. Shared
    // definitions can have different input clocks at each absolute instance;
    // only the shared enable is routed, so no clock export or grouping is needed.
    for (auto &path : paths)
      if (!aliases.root(module, sinkClock, path)) {
        error = "trigger sink clock needs an unconditional top input Clock alias on every instance path";
        return failure();
      }
    if (!dominance.properlyDominates(sinkClock.getValue(), node.getOperation())) {
      error = "trigger sink clock must dominate its node declaration"; return failure();
    }
    nodes.push_back({node, sinkClock});
    for (auto module : pathModules) sinkModules[module] = true;
  }
  // All unsupported scope/type/clock cases have been rejected before mutation.
  struct RoutedEvent { Value value; std::string name; };
  // Multiple uses of one source produce separate SSA values in their parent.
  // Keep those values by module and annotation until they reach top accounting.
  llvm::DenseMap<Operation *, llvm::DenseMap<unsigned, SmallVector<RoutedEvent>>> routed;
  SmallVector<Operation *> routingOrder;
  for (auto &[operation, indices] : childEvents) routingOrder.push_back(operation);
  // An ancestor can own sources as well as relay descendant events. Process
  // descendants first, then append all ancestor exports in one replacement.
  llvm::stable_sort(routingOrder, [&](Operation *a, Operation *b) { return depths[a] > depths[b]; });
  for (auto operation : routingOrder) {
    auto &indices = childEvents[operation];
    auto child = cast<FModuleOp>(operation);
    circt::Namespace childNames;
    for (auto name : child.getPortNamesAttr()) childNames.newName(cast<StringAttr>(name).getValue());
    child.walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("name")) childNames.newName(name.getValue());
    });
    ImplicitLocOpBuilder childBuilder(child.getLoc(), circuit.getContext());
    childBuilder.setInsertionPointToEnd(child.getBodyBlock());
    SmallVector<std::pair<unsigned, PortInfo>> added;
    SmallVector<RoutedEvent> signals;
    SmallVector<unsigned> signalIndices;
    unsigned oldPorts = child.getNumPorts();
    for (unsigned index : indices) {
      auto &event = events[index];
      auto localSignals = routed[operation].lookup(index);
      if (localSignals.empty()) {
        Value signal = getValueByFieldID(childBuilder, event.event.getValue(), event.event.getFieldID());
        std::string signalName = event.name;
        if (event.reset) {
          Value reset = getValueByFieldID(childBuilder, event.reset.getValue(), event.reset.getFieldID());
          Value active = childBuilder.create<NotPrimOp>(reset);
          signalName = childNames.newName(event.name + "_masked");
          signal = childBuilder.create<NodeOp>(childBuilder.create<AndPrimOp>(active, signal),
                                              childBuilder.getStringAttr(signalName)).getResult();
        }
        localSignals.push_back({signal, signalName});
      }
      for (auto &signal : localSignals) {
        auto portName = childNames.newName("simulationTrigger_" + signal.name);
        added.push_back({oldPorts, PortInfo(childBuilder.getStringAttr(portName),
            UIntType::get(circuit.getContext(), 1), Direction::Out)});
        signals.push_back(signal);
        signalIndices.push_back(index);
      }
    }
    child.insertPorts(added);
    for (unsigned i = 0; i < signals.size(); ++i)
      childBuilder.create<StrictConnectOp>(child.getBodyBlock()->getArgument(oldPorts + i), signals[i].value);
    for (auto instance : sourceParentInstances.lookup(operation)) {
      auto replacement = instance.cloneAndInsertPorts(added);
      for (auto attr : instance->getAttrs())
        if (!replacement->hasAttr(attr.getName())) replacement->setAttr(attr.getName(), attr.getValue());
      for (unsigned i = 0; i < oldPorts; ++i)
        instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
      auto parent = instance->getParentOfType<FModuleOp>();
      for (unsigned i = 0; i < signals.size(); ++i) {
        // Preserve multiplicity through relays: simultaneous sibling events
        // count separately, after each source instance applies its reset mask.
        routed[parent][signalIndices[i]].push_back({replacement.getResult(oldPorts + i),
          (instance.getName() + "_" + signals[i].name).str()});
      }
      // Source exports may replace an ancestor also used by a sink route.
      // Keep every sink handle live before erasing that original instance.
      for (auto &sinkInstance : sinkParentInstances[operation])
        if (sinkInstance == instance) sinkInstance = replacement;
      instance.erase();
    }
  }
  circt::Namespace names;
  for (auto name : top.getPortNamesAttr()) names.newName(cast<StringAttr>(name).getValue());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
  });
  OpBuilder b(circuit.getContext());
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto loc = top.getLoc();
  // Scala normalizes aggregate input ports before BridgeTopWiring chooses the
  // root and uses the flattened leaf name for local accounting. CIRCT keeps
  // the aggregate and materializes its Clock leaf only after atomic preflight.
  Value baseClock = getValueByFieldID(ImplicitLocOpBuilder(loc, b), baseField.getValue(),
                                    baseField.getFieldID());
  auto named = [&](Value value, StringRef name) -> Value {
    return b.create<NodeOp>(loc, value, b.getStringAttr(names.newName(name))).getResult();
  };
  auto reg = [&](unsigned width, StringRef name, Value domain) -> Value {
    // RegZeroPreset in Scala has reset=0/init=self, with no preset annotation.
    return b.create<RegOp>(loc, UIntType::get(b.getContext(), width), domain,
                           names.newName(name)).getResult();
  };
  struct DomainSignals { SmallVector<Value> credits, debits; };
  llvm::MapVector<circt::FieldRef, DomainSignals> domainSignals;
  for (auto [index, event] : llvm::enumerate(events)) {
    auto &domain = domainSignals[event.clock];
    auto &signals = event.credit ? domain.credits : domain.debits;
    if (auto exported = routed[top].find(index); exported != routed[top].end()) {
      for (auto &signal : exported->second) signals.push_back(signal.value);
      continue;
    }
    Value signal = getValueByFieldID(ImplicitLocOpBuilder(loc, b),
                                    event.event.getValue(), event.event.getFieldID());
    if (event.reset) {
      Value reset = getValueByFieldID(ImplicitLocOpBuilder(loc, b),
                                     event.reset.getValue(), event.reset.getFieldID());
      Value active = b.create<NotPrimOp>(loc, reset);
      signal = named(b.create<AndPrimOp>(loc, active, signal), event.name + "_masked");
    }
    signals.push_back(signal);
  }
  auto reduce = [&](ArrayRef<Value> signals, StringRef stem) -> Value {
    // Scala DensePrefixSum uses the previous layer for every addition at an
    // offset. A ripple sum would grow FIRRTL widths differently for N > 3.
    SmallVector<Value> layer(signals);
    for (size_t offset = 1; offset < layer.size(); offset *= 2) {
      SmallVector<Value> next(layer);
      for (size_t i = offset; i < layer.size(); ++i)
        next[i] = named(b.create<AddPrimOp>(loc, layer[i - offset], layer[i]),
                        stem.str() + "_sum");
      layer.swap(next);
    }
    return layer.back();
  };
  auto local = [&](ArrayRef<Value> signals, const std::string &stem, Value clock) -> Value {
    Value signal = reduce(signals, stem);
    Value count = reg(16, stem, clock);
    Value next = named(b.create<AddPrimOp>(loc, count, signal), stem + "_next");
    Value truncated = b.create<BitsPrimOp>(loc, next, 15, 0);
    b.create<StrictConnectOp>(loc, count, truncated);
    Value s1 = reg(16, stem + "_next_count_sync_s1", baseClock);
    Value s2 = reg(16, stem + "_next_count_sync_s2", baseClock);
    b.create<StrictConnectOp>(loc, s1, truncated);
    b.create<StrictConnectOp>(loc, s2, s1);
    // SFC infers UInt<17> subtraction, including the underflow bit at wrap.
    return named(b.create<SubPrimOp>(loc, s1, s2), stem + "_next_diff");
  };
  SmallVector<Value> creditDiffs, debitDiffs;
  for (auto &[root, signals] : domainSignals) {
    auto clockName = getFieldName(root, /*nameSafe=*/true).first;
    Value clock = getValueByFieldID(ImplicitLocOpBuilder(loc, b),
                                   root.getValue(), root.getFieldID());
    creditDiffs.push_back(local(signals.credits, clockName + "_credits", clock));
    debitDiffs.push_back(local(signals.debits, clockName + "_debits", clock));
  }
  // Reduce the UInt<17> differences without truncation. Scala retains the
  // underflow bit for each domain before forming the full global NEXT value.
  Value creditDiff = reduce(creditDiffs, "totalCredits");
  Value debitDiff = reduce(debitDiffs, "totalDebits");
  auto total = [&](Value diff, StringRef name) -> Value {
    Value count = reg(32, name, baseClock);
    Value next = named(b.create<AddPrimOp>(loc, count, diff), name.str() + "_next");
    b.create<StrictConnectOp>(loc, count, b.create<BitsPrimOp>(loc, next, 31, 0));
    return next;
  };
  Value creditNext = total(creditDiff, "totalCredits");
  Value debitNext = total(debitDiff, "totalDebits");
  // Compare full UInt<33> NEXT values; comparing truncated state changes wrap semantics.
  Value enable = named(b.create<NEQPrimOp>(loc, creditNext, debitNext), "trigger_source");
  // WiringTransform carries one shared trigger net into every module on a
  // sink's route. Source exports have already replaced some instances; use
  // their current handles, preserving those exports when adding input ports.
  SmallVector<Operation *> sinkOrder;
  for (auto &[operation, unused] : sinkModules) sinkOrder.push_back(operation);
  llvm::stable_sort(sinkOrder, [&](Operation *a, Operation *b) { return depths[a] < depths[b]; });
  llvm::DenseMap<Operation *, Value> sinkInputs;
  auto moduleNamespace = [&](FModuleOp module, circt::Namespace &ns) {
    for (auto name : module.getPortNamesAttr()) ns.newName(cast<StringAttr>(name).getValue());
    module.walk([&](Operation *op) {
      if (auto name = op->getAttrOfType<StringAttr>("name")) ns.newName(name.getValue());
    });
  };
  // Scala WiringTransform names the final sink input by its wiring key and
  // intermediate inputs by the source declaration. Namespace collisions are
  // resolved independently in each module.
  llvm::DenseSet<Operation *> sinkDefinitions;
  for (auto [node, unused] : nodes) sinkDefinitions.insert(node->getParentOp());
  for (auto operation : sinkOrder) {
    auto module = cast<FModuleOp>(operation);
    circt::Namespace ns; moduleNamespace(module, ns);
    unsigned oldPorts = module.getNumPorts();
    SmallVector<std::pair<unsigned, PortInfo>> added{{oldPorts,
      PortInfo(b.getStringAttr(ns.newName(sinkDefinitions.contains(operation) ? "trigger_sink" :
          enable.getDefiningOp<NodeOp>().getName())), UIntType::get(b.getContext(), 1), Direction::In)}};
    module.insertPorts(added);
    sinkInputs[operation] = module.getBodyBlock()->getArgument(oldPorts);
    for (auto instance : sinkParentInstances[operation]) {
      auto parent = instance->getParentOfType<FModuleOp>();
      auto replacement = instance.cloneAndInsertPorts(added);
      for (auto attr : instance->getAttrs())
        if (!replacement->hasAttr(attr.getName())) replacement->setAttr(attr.getName(), attr.getValue());
      for (unsigned i = 0; i < oldPorts; ++i)
        instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
      instance.erase();
      b.setInsertionPointToEnd(parent.getBodyBlock());
      b.create<StrictConnectOp>(loc, replacement.getResult(oldPorts),
                               parent == top ? enable : sinkInputs.lookup(parent));
    }
  }
  for (auto [node, sinkClock] : nodes) {
    auto module = node->getParentOfType<FModuleOp>();
    circt::Namespace ns;
    if (module != top) moduleNamespace(module, ns);
    b.setInsertionPoint(node);
    Value domain = getValueByFieldID(ImplicitLocOpBuilder(loc, b), sinkClock.getValue(),
                                   sinkClock.getFieldID());
    Value sync = b.create<RegOp>(loc, UIntType::get(b.getContext(), 1), domain,
                                (module == top ? names : ns).newName("trigger_sync")).getResult();
    b.setInsertionPointToEnd(module.getBodyBlock());
    b.create<StrictConnectOp>(loc, sync, module == top ? enable : sinkInputs.lookup(module));
    node->setOperand(0, sync);
  }
  consumed = sources.size();
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), retained));
  return success();
}
