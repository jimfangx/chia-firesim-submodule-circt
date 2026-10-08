// See LICENSE for license details.
#include "goldengate/LowerTypes.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "circt/Dialect/HW/InnerSymbolNamespace.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/DenseSet.h"
#include <array>

using namespace circt::firrtl;
using namespace mlir;

namespace {
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
  // ready/valid) follow SFC RTRenamer.exact. Keep endpoint indices so repeated
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
    const bool exact = autoCounter || triggerSource || triggerSink || channelConnection || channelPorts;
    const StringRef kind = clockChannel ? "TargetClockChannel" :
                           pipeChannel ? "PipeChannel" :
                           reverseChannel ? "DecoupledReverseChannel" :
                           forwardChannel ? "DecoupledForwardChannel" :
                           channelPorts ? "FAMEChannelPortsAnnotation" :
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
      auto local = spelling.getValue().split('>').second;
      std::string resolutionError;
      auto target = resolveAnnotationTarget(circuit, spelling.getValue(), resolutionError);
      if (target && target->port) {
        auto type = cast<FIRRTLBaseType>(circt::hw::FieldIdImpl::getFinalTypeByFieldID(
            target->module.getPortType(*target->port), *target->fieldID));
        if (exact && !type.isGround()) {
          error = kind.str() + " " + member.str() +
                  " must select a ground value: " + spelling.getValue().str();
          return failure();
        }
        collectGroundTargets(type, target->module, *target->port, nullptr,
                             *target->fieldID, plan.targets);
      } else if (auto internal = resolveInternalFieldTarget(circuit, spelling.getValue(),
                                                            resolutionError)) {
        if (exact && !internal->type.isGround()) {
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
        if (exact || !resolveInternalAnnotationTarget(circuit, spelling.getValue(),
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
      replacements[index].push_back(std::move(plan));
      return success();
    };
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
  passes.addNestedPass<CircuitOp>(createLowerFIRRTLTypesPass());
  if (failed(passes.run(module))) {
    error = "CIRCT LowerTypes failed";
    return failure();
  }

  uniquifyLoweredNames(circuit);
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
  auto loweredSpelling = [&](GroundTarget replacement) -> StringAttr {
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
    auto spelling = "~" + circuitName + "|" +
        replacement.module.getModuleName().str() + ">" + name.getValue().str();
    return StringAttr::get(module.getContext(), spelling);
  };
  for (auto [index, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    const TargetPlan *events = nullptr;
    for (auto &plan : replacements[index]) {
      if (plan.member == "target" && !plan.element && !plan.channelInfo) {
        events = &plan;
        continue;
      }
      auto spelling = loweredSpelling(plan.targets.front());
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
      auto spelling = loweredSpelling(replacement);
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
  PassManager passes(module.getContext());
  passes.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
  if (failed(passes.run(module))) {
    error = "CIRCT FAME input ExpandWhens failed";
    return failure();
  }
  return success();
}
