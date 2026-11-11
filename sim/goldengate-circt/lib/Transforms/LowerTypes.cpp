// See LICENSE for license details.
#include "goldengate/LowerTypes.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/HierarchicalTargets.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "circt/Dialect/HW/InnerSymbolNamespace.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/DenseSet.h"
#include <array>
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
// SFC DestructTypes.uniquify reserves intermediate bundle/vector prefixes as
// well as ground leaves. CIRCT need not reserve these names: instance results
// are SSA values rather than a flattened instance bundle. Preserve SFC's
// instance namespace nevertheless, so channel names and hierarchical targets
// agree when an extracted peer occupies one of those intermediate prefixes.
struct SFCNames {
  std::string root;
  SmallVector<std::string> all, leaves;
};
bool sfcZeroWidthType(FIRRTLBaseType type) {
  if (auto bundle = dyn_cast<BundleType>(type))
    return llvm::all_of(bundle.getElements(), [](auto element) {
      return sfcZeroWidthType(element.type);
    });
  if (auto vector = dyn_cast<FVectorType>(type))
    return vector.getNumElements() == 0 || sfcZeroWidthType(vector.getElementType());
  return type.getBitWidthOrSentinel() == 0;
}
SFCNames sfcNames(StringRef name, FIRRTLBaseType type,
                  std::set<std::string> &names,
                  const std::set<std::string> &reserved = {},
                  bool protectInstanceRoot = false) {
  // SFC ZeroWidth precedes LowerTypes. Deleted fields reserve neither ground
  // names nor intermediate prefixes, even when their widths were inferred.
  if (sfcZeroWidthType(type)) return {name.str(), {}, {}};
  std::string prefix = name.str();
  while (reserved.count(prefix)) prefix += '_';
  SmallVector<std::pair<std::string, FIRRTLBaseType>> children;
  if (auto bundle = dyn_cast<BundleType>(type)) {
    for (auto element : bundle.getElements())
      if (!sfcZeroWidthType(element.type))
        children.emplace_back(element.name.getValue().str(), element.type);
  } else if (auto vector = dyn_cast<FVectorType>(type)) {
    for (unsigned i = 0; i < vector.getNumElements(); ++i)
      children.emplace_back(std::to_string(i), vector.getElementType());
  } else return {prefix, {prefix}, {prefix}};
  std::set<std::string> local;
  for (auto &child : children) local.insert(child.first);
  SmallVector<std::string> suffixes, leaves;
  for (auto &child : children) {
    auto lowered = sfcNames(child.first, child.second, local);
    for (auto &suffix : lowered.all) suffixes.push_back("_" + suffix);
    for (auto &leaf : lowered.leaves) leaves.push_back("_" + leaf);
  }
  // SFC only checks suffixes here. A renamed instance root may nevertheless
  // collide with an existing declaration; keep the native IR valid in that
  // corner instead of reproducing SFC's duplicate-instance-name bug.
  while ((protectInstanceRoot && prefix != name && names.count(prefix)) ||
         llvm::any_of(suffixes, [&](const std::string &suffix) {
    return names.count(prefix + suffix);
  })) prefix += '_';
  if (prefix != name) {
    if (!reserved.count(name.str())) names.erase(name.str());
    names.insert(prefix);
  }
  SFCNames result{prefix, {}, {}};
  for (auto &suffix : suffixes) result.all.push_back(prefix + suffix);
  result.all.push_back(prefix);
  for (auto &leaf : leaves) result.leaves.push_back(prefix + leaf);
  return result;
}
struct InstanceRename {
  std::string name, child;
};
using InstanceRenames =
    std::map<std::pair<std::string, std::string>, InstanceRename>;
struct InstancePrefixPlan {
  FModuleLike owner;
  StringAttr symbol;
  std::string original, desired;
  bool temporary;
};

SmallVector<InstancePrefixPlan> uniquifySFCInstancePrefixes(CircuitOp circuit) {
  SmallVector<InstancePrefixPlan> plans;
  circt::hw::InnerSymbolNamespaceCollection namespaces;
  for (auto owner : circuit.getOps<FModuleLike>()) {
    // SFC lowers ports independently before constructing the body namespace.
    std::set<std::string> portNamespace, ports;
    for (unsigned i = 0; i < owner.getNumPorts(); ++i)
      if (auto type = dyn_cast<FIRRTLBaseType>(owner.getPortType(i)))
        if (!sfcZeroWidthType(type))
          portNamespace.insert(owner.getPortName(i).str());
    for (unsigned i = 0; i < owner.getNumPorts(); ++i) {
      auto type = dyn_cast<FIRRTLBaseType>(owner.getPortType(i));
      if (!type) continue;
      auto lowered = sfcNames(owner.getPortName(i), type, portNamespace);
      ports.insert(lowered.leaves.begin(), lowered.leaves.end());
    }
    auto names = ports;
    owner->walk([&](Operation *op) {
      if (isa<NodeOp, WireOp, RegOp, RegResetOp, MemOp, InstanceOp,
              InstanceChoiceOp>(op)) {
        if (isa<NodeOp, WireOp, RegOp, RegResetOp>(op) &&
            sfcZeroWidthType(cast<FIRRTLBaseType>(op->getResult(0).getType())))
          return;
        names.insert(op->getAttrOfType<StringAttr>("name").getValue().str());
      }
    });
    owner->walk([&](InstanceOp instance) {
      SmallVector<BundleType::BundleElement> fields;
      for (unsigned i = 0; i < instance.getNumResults(); ++i) {
        auto type = dyn_cast<FIRRTLBaseType>(instance.getResult(i).getType());
        if (!type) return;
        fields.push_back({cast<StringAttr>(instance.getPortNamesAttr()[i]),
                          false, type});
      }
      auto original = instance.getName().str();
      auto lowered = sfcNames(original,
          BundleType::get(circuit.getContext(), fields), names, ports, true);
      // CIRCT retains zero-width ports at this boundary and can independently
      // rename an instance for a prefix SFC's ZeroWidth has already deleted.
      // Carry the planned name through its supported native inner identity;
      // CIRCT's lowering does not retain arbitrary operation attributes.
      auto symbols = instance.getInnerSymAttr();
      auto symbol = symbols ? symbols.getSymIfExists(0) : StringAttr();
      bool temporary = !symbol;
      if (temporary) {
        symbol = StringAttr::get(circuit.getContext(),
                                  namespaces[owner].newName("gg_lower_instance"));
        SmallVector<circt::hw::InnerSymPropertiesAttr> properties;
        if (symbols) llvm::append_range(properties, symbols.getProps());
        properties.push_back(circt::hw::InnerSymPropertiesAttr::get(
            circuit.getContext(), symbol, 0,
            StringAttr::get(circuit.getContext(), "private")));
        instance.setInnerSymAttr(circt::hw::InnerSymAttr::get(circuit.getContext(), properties));
      }
      plans.push_back({owner, symbol, original, lowered.root, temporary});
      if (lowered.root == original) return;
      instance.setNameAttr(StringAttr::get(circuit.getContext(), lowered.root));
    });
  }
  return plans;
}

// Rewrite complete target tokens, following the parent module at every
// hierarchical edge. Never replace substrings in printf text, constructor
// payloads, global channel names, or another module's identically named pin.
StringAttr renameInstanceTarget(StringAttr attr, StringRef circuit,
                                const InstanceRenames &renames) {
  auto text = attr.getValue();
  if (!text.consume_front("~")) return attr;
  auto [targetCircuit, local] = text.split('|');
  if (targetCircuit != circuit || local.empty()) return attr;
  auto [hierarchy, reference] = local.split('>');
  SmallVector<StringRef> path;
  hierarchy.split(path, '/');
  if (path.empty() || path.front().empty()) return attr;
  std::string parent = path.front().str();
  std::string result = "~" + circuit.str() + "|" + parent;
  bool changed = false;
  for (auto edge : llvm::drop_begin(path)) {
    auto [instance, child] = edge.split(':');
    if (instance.empty() || child.empty() || child.contains(':')) return attr;
    auto found = renames.find({parent, instance.str()});
    if (found != renames.end() && found->second.child == child) {
      result += "/" + found->second.name + ":" + child.str();
      changed = true;
    } else result += "/" + edge.str();
    parent = child.str();
  }
  if (local.contains('>')) {
    if (reference.empty() || reference.contains('>')) return attr;
    auto end = reference.find_first_of(".[");
    auto root = reference.take_front(end);
    auto found = renames.find({parent, root.str()});
    result += ">";
    if (found != renames.end()) {
      result += found->second.name + reference.drop_front(root.size()).str();
      changed = true;
    } else result += reference.str();
  }
  return changed ? StringAttr::get(attr.getContext(), result) : attr;
}

Attribute renameInstanceSelectors(Attribute attr, StringRef circuit,
                                  const InstanceRenames &renames) {
  if (renames.empty()) return attr;
  Annotation annotation(attr);
  auto rewrite = [&](StringRef member) {
    if (auto target = annotation.getMember<StringAttr>(member))
      annotation.setMember(member, renameInstanceTarget(target, circuit, renames));
    else if (auto targets = annotation.getMember<ArrayAttr>(member)) {
      SmallVector<Attribute> updated;
      for (auto element : targets)
        updated.push_back(isa<StringAttr>(element) ?
            renameInstanceTarget(cast<StringAttr>(element), circuit, renames) : element);
      annotation.setMember(member, ArrayAttr::get(attr.getContext(), updated));
    }
  };
  // SingleTarget/Targets annotations have these standard schema members,
  // including user-defined classes. Other selectors are class-specific.
  rewrite("target");
  rewrite("targets");
  using A = goldengate::AnnotationClasses;
  if (annotation.isClass(A::ChannelConnection)) {
    for (StringRef member : {"clock", "sources", "sinks"}) rewrite(member);
    if (auto info = annotation.getMember<DictionaryAttr>("channelInfo")) {
      Annotation channel(info);
      if (channel.isClass(A::DecoupledForwardChannel)) {
        for (StringRef member : {"readySink", "validSource", "readySource", "validSink"})
          if (auto target = channel.getMember<StringAttr>(member))
            channel.setMember(member, renameInstanceTarget(target, circuit, renames));
        annotation.setMember("channelInfo", channel.getAttr());
      }
    }
  } else if (annotation.isClass(A::ChannelPorts)) {
    rewrite("clockPort"); rewrite("ports");
  } else if (annotation.isClass(A::ModelReadPort, A::ModelWritePort,
                                A::ModelReadWritePort)) {
    for (StringRef member : {"data", "mask", "addr", "en", "wmode", "rdata", "wdata", "wmask"})
      rewrite(member);
  } else if (annotation.isClass(A::AutoCounter, A::InternalAutoCounter,
                               A::TriggerSource, A::InternalTriggerSource,
                               A::TriggerSink, A::InternalTriggerSink)) {
    rewrite("clock"); rewrite("reset");
  }
  return annotation.getAttr();
}

// SFC DestructTypes records each aggregate reference as referring to all of
// its ground children. Walk the selected CIRCT subtype in declaration order,
// including vector indices, to apply that one-to-many rename to raw metadata.
struct GroundTarget {
  FModuleLike module;
  std::optional<unsigned> port;
  Operation *declaration;
  uint64_t fieldID;
  StringAttr symbol;
};

void collectGroundTargets(FIRRTLBaseType type, FModuleLike module,
                          std::optional<unsigned> port, Operation *declaration,
                          uint64_t fieldID,
                          SmallVectorImpl<GroundTarget> &targets) {
  if (auto bundle = dyn_cast<BundleType>(type)) {
    for (auto [i, element] : llvm::enumerate(bundle.getElements()))
      collectGroundTargets(element.type, module, port, declaration,
                           fieldID + bundle.getFieldID(i), targets);
  } else if (auto vector = dyn_cast<FVectorType>(type)) {
    for (unsigned i = 0; i < vector.getNumElements(); ++i)
      collectGroundTargets(vector.getElementType(), module, port, declaration,
                           fieldID + vector.getFieldID(i), targets);
  } else if (type.getBitWidthOrSentinel() != 0) {
    // SFC RemoveZeroWidth deletes these references before DestructTypes.
    // Fanout annotations therefore expand only to surviving ground leaves.
    targets.push_back({module, port, declaration, fieldID, {}});
  }
}
// LowerTypes can leave duplicate declaration/port names even though its
// printed SSA names are distinct. Reserve all existing spellings, then rename
// duplicates in declaration order using CIRCT's namespace. Instances bind by
// result index; update their portNames so exported connects use the same leaves.
void uniquifyLoweredNames(CircuitOp circuit) {
  llvm::DenseMap<StringAttr, ArrayAttr> renamed;
  for (auto owner : circuit.getOps<FModuleLike>()) {
    circt::Namespace names;
    llvm::DenseSet<Attribute> seen;
    auto original = owner.getPortNamesAttr();
    for (auto attr : original)
      if (seen.insert(attr).second)
        names.newName(cast<StringAttr>(attr).getValue());
    SmallVector<Operation *> declarations;
    owner->walk([&](Operation *op) {
      if (isa<NodeOp, WireOp, RegOp, RegResetOp, MemOp, InstanceOp,
              InstanceChoiceOp>(op)) {
        declarations.push_back(op);
        auto name = op->getAttrOfType<StringAttr>("name");
        if (seen.insert(name).second)
          names.newName(name.getValue());
      }
    });
    seen.clear();
    SmallVector<Attribute> ports;
    for (auto attr : original)
      ports.push_back(seen.insert(attr).second ? attr :
          StringAttr::get(circuit.getContext(),
                         names.newName(cast<StringAttr>(attr).getValue())));
    for (auto *op : declarations) {
      auto name = op->getAttrOfType<StringAttr>("name");
      if (!seen.insert(name).second)
        op->setAttr("name", StringAttr::get(circuit.getContext(),
                                          names.newName(name.getValue())));
    }
    auto updated = ArrayAttr::get(circuit.getContext(), ports);
    if (updated != original) {
      owner->setAttr("portNames", updated);
      renamed[owner.getModuleNameAttr()] = updated;
    }
  }
  circuit.walk([&](InstanceOp instance) {
    if (auto ports = renamed.lookup(instance.getModuleNameAttr().getAttr()))
      instance.setPortNamesAttr(ports);
  });
  circuit.walk([&](InstanceChoiceOp instance) {
    // All alternatives have the same interface, hence the same deterministic
    // port-name uniquification. The first entry is the default alternative.
    auto name = instance.getDefaultTargetAttr().getAttr();
    if (auto ports = renamed.lookup(name))
      instance.setPortNamesAttr(ports);
  });
}
} // namespace

LogicalResult goldengate::lowerTypesWithRetainedTargets(
    ModuleOp module, CircuitOp circuit, std::string &error,
    RetainedTargetScope scope) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "LowerTypes needs retained annotations";
    return failure();
  }
  const std::string circuitName = circuit.getName().str();
  // DontTouch, host/global reset signal and FPGA debug targets may fan out.
  // Trigger/AutoCounter scalar members and channel endpoints (including nested
  // ready/valid) and SRAM model port members follow SFC RTRenamer.exact.
  // Keep endpoint indices so repeated
  // references and clock schedule order survive.
  // An empty fanout plan removes an empty aggregate annotation; no plan
  // preserves a member whose identity does not need transferring.
  struct TargetPlan {
    StringRef member;
    std::optional<unsigned> element;
    SmallVector<GroundTarget> targets;
    bool channelInfo;
    bool legacyComponent = false;
    bool exact = false;
    std::string hierarchy;
  };
  SmallVector<SmallVector<TargetPlan>> replacements(raw.size());
  auto isHostSignal = [](Annotation annotation) {
    return annotation.isClass(AnnotationClasses::HostClock) ||
        annotation.isClass(AnnotationClasses::HostReset) ||
        annotation.isClass(AnnotationClasses::HostClockSource) ||
        annotation.isClass(AnnotationClasses::HostClockSink);
  };
  auto isFpgaDebug = [](Annotation annotation) {
    return annotation.isClass(AnnotationClasses::FpgaDebug) ||
        annotation.isClass(AnnotationClasses::InternalFpgaDebug);
  };
  auto isGlobalReset = [](Annotation annotation) {
    return annotation.isClass(AnnotationClasses::GlobalResetSource) ||
        annotation.isClass(AnnotationClasses::GlobalResetSink) ||
        annotation.isClass(AnnotationClasses::PublicGlobalResetSource) ||
        annotation.isClass(AnnotationClasses::PublicGlobalResetSink);
  };
  for (auto [index, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    const bool dontTouch = annotation.isClass(AnnotationClasses::DontTouch);
    const bool hostSignal = isHostSignal(annotation);
    const bool fpgaDebug = isFpgaDebug(annotation);
    const bool globalReset = isGlobalReset(annotation);
    if (scope == RetainedTargetScope::FpgaDebugOnly && !fpgaDebug)
      continue;
    const bool autoCounter = annotation.isClass(AnnotationClasses::AutoCounter) ||
        annotation.isClass(AnnotationClasses::InternalAutoCounter);
    const bool triggerSource = annotation.isClass(AnnotationClasses::TriggerSource) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSource);
    const bool triggerSink = annotation.isClass(AnnotationClasses::TriggerSink) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSink);
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    const bool clockChannel = annotation.isClass(AnnotationClasses::ChannelConnection) &&
        info && Annotation(info).isClass(AnnotationClasses::TargetClockChannel);
    const bool pipeChannel = annotation.isClass(AnnotationClasses::ChannelConnection) &&
        info && Annotation(info).isClass(AnnotationClasses::PipeChannel);
    const bool reverseChannel = annotation.isClass(AnnotationClasses::ChannelConnection) &&
        info && Annotation(info).isClass(AnnotationClasses::DecoupledReverseChannel);
    const bool forwardChannel = annotation.isClass(AnnotationClasses::ChannelConnection) &&
        info && Annotation(info).isClass(AnnotationClasses::DecoupledForwardChannel);
    const bool channelConnection = clockChannel || pipeChannel || reverseChannel || forwardChannel;
    const bool channelPorts = annotation.isClass(AnnotationClasses::ChannelPorts);
    const bool readPort = annotation.isClass(AnnotationClasses::ModelReadPort);
    const bool writePort = annotation.isClass(AnnotationClasses::ModelWritePort);
    const bool readWritePort = annotation.isClass(AnnotationClasses::ModelReadWritePort);
    const bool memoryPort = readPort || writePort || readWritePort;
    const bool exact = autoCounter || triggerSource || triggerSink ||
                       channelConnection || channelPorts || memoryPort;
    const StringRef kind = clockChannel ? "TargetClockChannel" :
                           pipeChannel ? "PipeChannel" :
                           reverseChannel ? "DecoupledReverseChannel" :
                           forwardChannel ? "DecoupledForwardChannel" :
                           channelPorts ? "FAMEChannelPortsAnnotation" :
                           readPort ? "ModelReadPort" :
                           writePort ? "ModelWritePort" :
                           readWritePort ? "ModelReadWritePort" :
                           autoCounter ? "AutoCounter" : "Trigger";
    if (!dontTouch && !hostSignal && !fpgaDebug && !globalReset && !exact)
      continue;
    auto planTarget = [&](StringRef member, StringAttr spelling,
                          std::optional<unsigned> element = std::nullopt,
                          bool channelInfo = false) -> LogicalResult {
      TargetPlan plan{member, element, {}, channelInfo};
      plan.exact = exact;
      // Debug annotations use SFC ComponentName, serialized as
      // circuit.module.component. Resolve that local identity through CIRCT
      // field IDs, then retain its original JSON representation on each leaf.
      // A modern reference spelling is also accepted at the native boundary.
      if (fpgaDebug && !spelling.getValue().starts_with("~")) {
        auto circuitAndLocal = spelling.getValue().split('.');
        auto moduleAndRef = circuitAndLocal.second.split('.');
        if (circuitAndLocal.first != circuitName ||
            moduleAndRef.first.empty() || moduleAndRef.second.empty()) {
          error = "invalid FPGA debug ComponentName: " + spelling.getValue().str();
          return failure();
        }
        plan.legacyComponent = true;
        spelling = StringAttr::get(module.getContext(), "~" + circuitName +
            "|" + moduleAndRef.first.str() + ">" + moduleAndRef.second.str());
      }
      std::string resolutionError;
      auto resolutionSpelling = spelling;
      if (spelling.getValue().split('>').first.contains('/')) {
        auto resolved = resolveHierarchicalReference(circuit, spelling.getValue(), resolutionError);
        if (!resolved) {
          error = "unresolved retained annotation hierarchy " + spelling.getValue().str() + ": " + resolutionError;
          return failure();
        }
        plan.hierarchy = resolved->hierarchy;
        resolutionSpelling = StringAttr::get(module.getContext(), resolved->localTarget);
      }
      auto local = resolutionSpelling.getValue().split('>').second;
      auto target = resolveAnnotationTarget(circuit, resolutionSpelling.getValue(), resolutionError);
      if (target && target->port) {
        auto type = cast<FIRRTLBaseType>(circt::hw::FieldIdImpl::getFinalTypeByFieldID(
            target->module.getPortType(*target->port), *target->fieldID));
        if (exact && !memoryPort && !type.isGround()) {
          error = kind.str() + " " + member.str() +
                  " must select a ground value: " + spelling.getValue().str();
          return failure();
        }
        collectGroundTargets(type, target->module, *target->port, nullptr,
                             *target->fieldID, plan.targets);
      } else if (auto internal = resolveInternalFieldTarget(circuit, resolutionSpelling.getValue(),
                                                            resolutionError)) {
        if (exact && !memoryPort && !internal->type.isGround()) {
          error = kind.str() + " " + member.str() +
                  " must select a ground value: " + spelling.getValue().str();
          return failure();
        }
        collectGroundTargets(internal->type, internal->module, std::nullopt,
                             internal->declaration, internal->fieldID, plan.targets);
      } else {
        // Unused ground triggers/global resets are consumed without resolution
        // when their wiring consumer has no source or no sinks.
        // Preserve ground AutoCounter events and root memory DontTouches as before.
        if (((autoCounter && member == "target") || triggerSource || triggerSink ||
             globalReset) &&
            !local.contains('.') && !local.contains('['))
          return success();
        if (exact || !resolveInternalAnnotationTarget(circuit, resolutionSpelling.getValue(),
                                                     resolutionError)) {
          error = "unresolved retained annotation " + member.str() + " " +
                  spelling.getValue().str() + ": " + resolutionError;
          return failure();
        }
        return success();
      }
      // RTRenamer.exact requires one surviving reference. An explicitly
      // zero-width endpoint has no rename after SFC RemoveZeroWidth. Reject it
      // before attaching symbols or changing IR, just like an aggregate endpoint.
      if (exact && plan.targets.empty()) {
        error = kind.str() + " " + member.str() +
                " selects a zero-width value: " + spelling.getValue().str();
        return failure();
      }
      if (memoryPort && plan.targets.size() != 1) {
        error = kind.str() + " " + member.str() +
                " must select exactly one ground value: " + spelling.getValue().str();
        return failure();
      }
      replacements[index].push_back(std::move(plan));
      return success();
    };
    if (memoryPort) {
      // LabelSRAMModels emits these selectors on the wrapper's aggregate
      // ports, before High/Middle -> Low lowering. Every member is required
      // and independently exact. Singleton bundles/vectors select one leaf;
      // data/mask aggregates must not fan out into multiple memory ports.
      // Use native identities, including collisions.
      const StringRef readMembers[]{"data", "addr", "en"};
      const StringRef writeMembers[]{"data", "mask", "addr", "en"};
      const StringRef readWriteMembers[]{"wmode", "rdata", "wdata", "wmask",
                                        "addr", "en"};
      ArrayRef<StringRef> members = readPort ? ArrayRef<StringRef>(readMembers)
          : writePort ? ArrayRef<StringRef>(writeMembers)
                      : ArrayRef<StringRef>(readWriteMembers);
      for (auto member : members) {
        auto spelling = annotation.getMember<StringAttr>(member);
        if (!spelling) {
          error = kind.str() + " " + member.str() + " is not a reference target";
          return failure();
        }
        if (failed(planTarget(member, spelling))) return failure();
      }
      continue;
    }
    if (channelPorts) {
      if (auto clock = annotation.getMember("clockPort")) {
        auto spelling = dyn_cast<StringAttr>(clock);
        if (!spelling) {
          error = kind.str() + " clockPort is not a reference target";
          return failure();
        }
        if (failed(planTarget("clockPort", spelling))) return failure();
      }
      auto ports = annotation.getMember<ArrayAttr>("ports");
      if (!ports) {
        error = kind.str() + " ports is not an array of reference targets";
        return failure();
      }
      for (auto [element, port] : llvm::enumerate(ports)) {
        auto spelling = dyn_cast<StringAttr>(port);
        if (!spelling) {
          error = kind.str() + " ports endpoint is not a reference target";
          return failure();
        }
        if (failed(planTarget("ports", spelling, element))) return failure();
      }
      continue;
    }
    if (channelConnection) {
      // Optional members are left absent; empty arrays remain empty. Rational
      // clocks, perClockMFMR, pipe latency and reverse channel info contain no
      // reference targets; ready endpoints are ordinary connection members.
      if (auto clock = annotation.getMember<StringAttr>("clock"))
        if (failed(planTarget("clock", clock))) return failure();
      for (StringRef member : {"sources", "sinks"}) {
        auto endpoints = annotation.getMember<ArrayAttr>(member);
        if (!endpoints) continue;
        for (auto [element, endpoint] : llvm::enumerate(endpoints)) {
          auto spelling = dyn_cast<StringAttr>(endpoint);
          if (!spelling) {
            error = kind.str() + " " + member.str() + " endpoint is not a reference target";
            return failure();
          }
          if (failed(planTarget(member, spelling, element))) return failure();
        }
      }
      if (forwardChannel)
        for (StringRef member : {"readySink", "validSource", "readySource", "validSink"}) {
          auto endpoint = info.get(member);
          if (!endpoint) continue;
          auto spelling = dyn_cast<StringAttr>(endpoint);
          if (!spelling) {
            error = kind.str() + " channelInfo." + member.str() +
                    " endpoint is not a reference target";
            return failure();
          }
          if (failed(planTarget(member, spelling, std::nullopt, true))) return failure();
        }
      continue;
    }
    const std::array<StringRef, 3> members{"target", "clock", "reset"};
    unsigned memberCount = triggerSink ? 2 : exact ? members.size() : 1;
    for (unsigned member = 0; member < memberCount; ++member) {
      auto spelling = annotation.getMember<StringAttr>(members[member]);
      // Missing optional metadata remains the consuming analysis's responsibility.
      if (!spelling && member != 0) continue;
      if (!spelling) {
        error = "retained target annotation has no target";
        return failure();
      }
      if (failed(planTarget(members[member], spelling))) return failure();
    }
  }

  // Materialize identities only after all retained selectors pass preflight.
  // Reuse existing leaf symbols, including their visibility and InnerRef users.
  // Temporary private leaf symbols carry annotations through CIRCT expansion
  // and namespace uniquification. Aggregate-root symbols would not survive
  // lowering, so never create them.
  circt::hw::InnerSymbolNamespaceCollection namespaces;
  llvm::DenseMap<Operation *, SmallVector<StringAttr>> temporarySymbols;
  for (auto &annotationPlan : replacements) {
    for (auto &replacement : annotationPlan) {
      for (auto &target : replacement.targets) {
        auto symbols = target.port
            ? target.module.getPortSymbolAttr(*target.port)
            : cast<circt::hw::InnerSymbolOpInterface>(target.declaration)
                  .getInnerSymAttr();
        if (symbols)
          target.symbol = symbols.getSymIfExists(target.fieldID);
        if (target.symbol)
          continue;
        target.symbol = StringAttr::get(module.getContext(),
            namespaces[target.module].newName("gg_lower_target"));
        SmallVector<circt::hw::InnerSymPropertiesAttr> properties;
        if (symbols)
          llvm::append_range(properties, symbols.getProps());
        properties.push_back(circt::hw::InnerSymPropertiesAttr::get(
            module.getContext(), target.symbol, target.fieldID,
            StringAttr::get(module.getContext(), "private")));
        auto updated = circt::hw::InnerSymAttr::get(module.getContext(), properties);
        if (target.port)
          target.module.setPortSymbolsAttr(*target.port, updated);
        else
          cast<circt::hw::InnerSymbolOpInterface>(target.declaration)
              .setInnerSymbolAttr(updated);
        temporarySymbols[target.module].push_back(target.symbol);
      }
    }
  }
  // Remove only identities introduced here, also on a downstream pass failure.
  // Existing symbol properties and native InnerRefs remain untouched.
  auto cleanup = llvm::make_scope_exit([&] {
    for (auto &entry : temporarySymbols) {
      auto owner = cast<FModuleLike>(entry.first);
      auto &names = entry.second;
      for (unsigned port = 0; port < owner.getNumPorts(); ++port) {
        auto symbols = owner.getPortSymbolAttr(port);
        if (!symbols)
          continue;
        SmallVector<circt::hw::InnerSymPropertiesAttr> properties;
        for (auto property : symbols)
          if (!llvm::is_contained(names, property.getName()))
            properties.push_back(property);
        if (properties.size() != symbols.size())
          owner.setPortSymbolsAttr(port,
              circt::hw::InnerSymAttr::get(module.getContext(), properties));
      }
      owner->walk([&](circt::hw::InnerSymbolOpInterface declaration) {
        auto symbols = declaration.getInnerSymAttr();
        if (!symbols)
          return;
        SmallVector<circt::hw::InnerSymPropertiesAttr> properties;
        for (auto property : symbols)
          if (!llvm::is_contained(names, property.getName()))
            properties.push_back(property);
        if (properties.size() != symbols.size()) {
          if (properties.empty())
            declaration->removeAttr(
                circt::hw::InnerSymbolTable::getInnerSymbolAttrName());
          else
            declaration.setInnerSymbolAttr(
                circt::hw::InnerSymAttr::get(module.getContext(), properties));
        }
      });
    }
  });

  PassManager passes(module.getContext());
  // The Scala target-lowering compiler resolves CHIRRTL, widths and resets
  // before Golden Gate analyzes ground ports. In particular, LowerTypes turns
  // dynamic vector reads into multibit_mux operations: their element widths
  // must already be known, since CIRCT InferWidths cannot infer those ops.
  passes.nest<CircuitOp>().addNestedPass<FModuleOp>(createLowerCHIRRTLPass());
  passes.addNestedPass<CircuitOp>(createInferWidthsPass());
  passes.addNestedPass<CircuitOp>(createInferResetsPass());
  if (failed(passes.run(module))) {
    error = "CIRCT LowerTypes input inference failed";
    return failure();
  }
  // Namespace planning needs final widths but the original aggregate shape.
  // In particular, an unknown-width port inferred as zero must not trigger an
  // instance rename based on a field that SFC deletes before flattening.
  auto instancePlans = uniquifySFCInstancePrefixes(circuit);
  auto cleanupPrefixes = llvm::make_scope_exit([&] {
    circt::hw::InnerSymbolTableCollection identities;
    for (auto &plan : instancePlans) {
      if (!plan.temporary) continue;
      auto target = identities.getInnerSymbolTable(plan.owner).lookup(plan.symbol);
      if (!target || target.isPort()) continue;
      auto declaration = dyn_cast<circt::hw::InnerSymbolOpInterface>(target.getOp());
      if (!declaration) continue;
      SmallVector<circt::hw::InnerSymPropertiesAttr> properties;
      for (auto property : declaration.getInnerSymAttr())
        if (property.getName() != plan.symbol) properties.push_back(property);
      if (properties.empty())
        declaration->removeAttr(circt::hw::InnerSymbolTable::getInnerSymbolAttrName());
      else declaration.setInnerSymbolAttr(circt::hw::InnerSymAttr::get(module.getContext(), properties));
    }
  });
  PassManager lowering(module.getContext());
  lowering.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  if (failed(lowering.run(module))) {
    error = "CIRCT LowerTypes failed";
    return failure();
  }

  circt::hw::InnerSymbolTableCollection instanceIdentities;
  for (auto &plan : instancePlans) {
    auto target = instanceIdentities.getInnerSymbolTable(plan.owner).lookup(plan.symbol);
    auto instance = target && !target.isPort() ? dyn_cast<InstanceOp>(target.getOp()) : InstanceOp();
    if (!instance || target.getField() != 0) {
      error = "LowerTypes did not preserve instance namespace identity " + plan.symbol.getValue().str();
      return failure();
    }
    instance.setNameAttr(StringAttr::get(module.getContext(), plan.desired));
  }

  uniquifyLoweredNames(circuit);
  // A flattened non-instance declaration can still occupy a planned root.
  // Bind raw hierarchy selectors to the final unique declaration identity,
  // never to a provisional prefix. Native InnerRefs do not depend on names.
  InstanceRenames instanceRenames;
  for (auto &plan : instancePlans) {
    auto target = instanceIdentities.getInnerSymbolTable(plan.owner).lookup(plan.symbol);
    auto instance = cast<InstanceOp>(target.getOp());
    if (plan.original != instance.getName())
      instanceRenames[{plan.owner.getModuleName().str(), plan.original}] =
          {instance.getName().str(), instance.getModuleName().str()};
  }
  circt::hw::InnerSymbolTableCollection tables;
  // Width inference can also discover a zero-width leaf. Resolve its final
  // CIRCT type through the same identity used for renaming, so it obeys the
  // deletion rule even when the input width was not known during preflight.
  for (auto &annotationPlan : replacements) {
    for (auto &plan : annotationPlan) {
      llvm::erase_if(plan.targets, [&](GroundTarget target) {
        auto lowered = tables.getInnerSymbolTable(target.module).lookup(target.symbol);
        if (!lowered || lowered.getField() != 0)
          return false; // The identity diagnostic below handles this case.
        auto type = lowered.isPort()
            ? target.module.getPortType(lowered.getPort())
            : cast<circt::hw::InnerSymbolOpInterface>(lowered.getOp())
                  .getTargetResult().getType();
        auto base = dyn_cast<FIRRTLBaseType>(type);
        return base && base.getBitWidthOrSentinel() == 0;
      });
      if (plan.exact && plan.targets.empty()) {
        error = "LowerTypes " + plan.member.str() +
                " selects an inferred zero-width value";
        return failure();
      }
    }
  }
  SmallVector<Attribute> rewritten;
  rewritten.reserve(raw.size());
  // Scala LowForm coalesces identical SingleTargetAnnotation leaves, including
  // overlapping aggregate/leaf selectors. Distinct payloads remain distinct.
  llvm::DenseSet<Attribute> singleTargets;
  auto appendAnnotation = [&](Attribute attr) {
    Annotation annotation(attr);
    if ((scope == RetainedTargetScope::FpgaDebugOnly && !isFpgaDebug(annotation)) ||
        (!isHostSignal(annotation) && !isFpgaDebug(annotation) &&
         !isGlobalReset(annotation)) ||
        singleTargets.insert(attr).second)
      rewritten.push_back(attr);
  };
  auto loweredSpelling = [&](GroundTarget replacement, StringRef hierarchy) -> StringAttr {
    auto lowered = tables.getInnerSymbolTable(replacement.module)
                       .lookup(replacement.symbol);
    if (!lowered || lowered.getField() != 0) {
      error = "LowerTypes did not preserve annotated ground identity " +
              replacement.symbol.getValue().str();
      return {};
    }
    FIRRTLBaseType type;
    StringAttr name;
    if (lowered.isPort()) {
      type = dyn_cast<FIRRTLBaseType>(
          replacement.module.getPortType(lowered.getPort()));
      name = replacement.module.getPortNameAttr(lowered.getPort());
    } else {
      auto declaration = cast<circt::hw::InnerSymbolOpInterface>(lowered.getOp());
      type = dyn_cast<FIRRTLBaseType>(declaration.getTargetResult().getType());
      name = declaration->getAttrOfType<StringAttr>("name");
    }
    if (!type || !type.isGround() || !name) {
      error = "LowerTypes annotated identity is not a named ground value";
      return {};
    }
    std::string prefix = "~" + circuitName + "|" + replacement.module.getModuleName().str();
    if (!hierarchy.empty())
      prefix = renameInstanceTarget(StringAttr::get(module.getContext(), hierarchy),
                                    circuitName, instanceRenames).getValue().str();
    auto spelling = prefix + ">" + name.getValue().str();
    return StringAttr::get(module.getContext(), spelling);
  };
  for (auto [index, attr] : llvm::enumerate(raw)) {
    // Rename original hierarchy selectors once. Ground selectors produced
    // below already carry final native identities and must not be interpreted
    // again as an old instance root of the same spelling.
    Annotation annotation(renameInstanceSelectors(attr, circuitName, instanceRenames));
    const TargetPlan *events = nullptr;
    for (auto &plan : replacements[index]) {
      if (plan.member == "target" && !plan.element && !plan.channelInfo) {
        events = &plan;
        continue;
      }
      auto spelling = loweredSpelling(plan.targets.front(), plan.hierarchy);
      if (!spelling) return failure();
      if (plan.channelInfo) {
        Annotation info(annotation.getMember<DictionaryAttr>("channelInfo"));
        info.setMember(plan.member, spelling);
        annotation.setMember("channelInfo", info.getAttr());
      } else if (plan.element) {
        auto endpoints = annotation.getMember<ArrayAttr>(plan.member);
        SmallVector<Attribute> updated(endpoints.begin(), endpoints.end());
        updated[*plan.element] = spelling;
        annotation.setMember(plan.member, ArrayAttr::get(module.getContext(), updated));
      } else annotation.setMember(plan.member, spelling);
    }
    if (!events) {
      appendAnnotation(annotation.getAttr());
      continue;
    }
    for (auto &replacement : events->targets) {
      auto spelling = loweredSpelling(replacement, events->hierarchy);
      if (!spelling) return failure();
      if (events->legacyComponent) {
        auto moduleAndRef =
            spelling.getValue().drop_front(1).split('|').second.split('>');
        spelling = StringAttr::get(module.getContext(), circuitName + "." +
            moduleAndRef.first.str() + "." + moduleAndRef.second.str());
      }
      Annotation leaf(annotation.getAttr());
      leaf.setMember("target", spelling);
      appendAnnotation(leaf.getAttr());
    }
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(module.getContext(),
                                                     rewritten));
  return success();
}

LogicalResult goldengate::normalizeFAMEInput(
    ModuleOp module, CircuitOp circuit, std::string &error) {
  if (failed(lowerTypesWithRetainedTargets(module, circuit, error)))
    return failure();
  // SFC's LowForm boundary resolves conditional and last-connect semantics
  // before FAME computes channel connectivity. A connect inside a when is
  // controlled by the predicate as well as its source. Let CIRCT build those
  // muxes after ground lowering, including priority between repeated connects.
  // SFC FIRRTL has no const type qualifier. Imported literals can nevertheless
  // give nested when predicates const UInt types (PTW way masks in BOOM and
  // Shuttle, and Gemmini conditions). Drop that qualifier before ExpandWhens
  // combines them with dynamic predicates; literal values and named identities
  // remain intact, and the generated conjunction has an ordinary Bool type.
  PassManager passes(module.getContext());
  passes.nest<CircuitOp>().addPass(createDropConstPass());
  passes.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
  if (failed(passes.run(module))) {
    error = "CIRCT FAME input ExpandWhens failed";
    return failure();
  }
  return success();
}
