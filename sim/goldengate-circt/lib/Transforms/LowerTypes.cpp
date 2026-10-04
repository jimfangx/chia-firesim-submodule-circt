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
  } else {
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
    ModuleOp module, CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "LowerTypes needs retained annotations";
    return failure();
  }
  const std::string circuitName = circuit.getName().str();
  // An engaged, empty plan removes an annotation on an empty aggregate;
  // an absent plan leaves unrelated targets untouched.
  // AutoCounter and trigger annotations use exact renames for event, clock
  // and optional reset references in SFC. Keep separate leaf identities for
  // event, clock and reset; only
  // DontTouch targets may expand into more than one annotation.
  const std::array<StringRef, 3> members{"target", "clock", "reset"};
  using MemberPlan = std::optional<SmallVector<GroundTarget>>;
  SmallVector<std::array<MemberPlan, 3>> replacements(raw.size());
  for (auto [index, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    const bool dontTouch = annotation.isClass(AnnotationClasses::DontTouch);
    const bool autoCounter = annotation.isClass(AnnotationClasses::AutoCounter) ||
        annotation.isClass(AnnotationClasses::InternalAutoCounter);
    const bool triggerSource = annotation.isClass(AnnotationClasses::TriggerSource) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSource);
    const bool triggerSink = annotation.isClass(AnnotationClasses::TriggerSink) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSink);
    const bool exact = autoCounter || triggerSource || triggerSink;
    const StringRef kind = autoCounter ? "AutoCounter" : "Trigger";
    if (!dontTouch && !exact)
      continue;
    unsigned memberCount = triggerSink ? 2 : exact ? members.size() : 1;
    for (unsigned member = 0; member < memberCount; ++member) {
      auto &replacement = replacements[index][member];
      auto spelling = annotation.getMember<StringAttr>(members[member]);
      // Missing optional metadata remains the responsibility of the consuming
      // AutoCounter/trigger analysis; partial handoffs still lower their event.
      if (!spelling && member != 0)
        continue;
      if (!spelling) {
        error = "retained target annotation has no target";
        return failure();
      }
      auto local = spelling.getValue().split('>').second;
      std::string resolutionError;
      auto target = resolveAnnotationTarget(circuit, spelling.getValue(),
                                            resolutionError);
      if (target && target->port) {
        auto type = cast<FIRRTLBaseType>(circt::hw::FieldIdImpl::getFinalTypeByFieldID(
            target->module.getPortType(*target->port), *target->fieldID));
        if (exact && !type.isGround()) {
          error = kind.str() + " " + members[member].str() +
                  " must select a ground value: " +
                  spelling.getValue().str();
          return failure();
        }
        replacement.emplace();
        collectGroundTargets(type, target->module, *target->port, nullptr,
                             *target->fieldID, *replacement);
        continue;
      }
      if (auto internal = resolveInternalFieldTarget(circuit, spelling.getValue(),
                                                     resolutionError)) {
        if (exact && !internal->type.isGround()) {
          error = kind.str() + " " + members[member].str() +
                  " must select a ground value: " +
                  spelling.getValue().str();
          return failure();
        }
        replacement.emplace();
        collectGroundTargets(internal->type, internal->module, std::nullopt,
                             internal->declaration, internal->fieldID,
                             *replacement);
        continue;
      }
      // Defer unresolved ground trigger metadata to TriggerWiring, which skips
      // reference resolution when no sources or sinks need hardware. Preserve
      // ground AutoCounter events and root memory DontTouches as before.
      if (((autoCounter && member == 0) || triggerSource || triggerSink) &&
          !local.contains('.') && !local.contains('['))
        continue;
      if (exact ||
          !resolveInternalAnnotationTarget(circuit, spelling.getValue(),
                                           resolutionError)) {
        error = "unresolved retained annotation " + members[member].str() + " " +
                spelling.getValue().str() + ": " + resolutionError;
        return failure();
      }
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
      if (!replacement)
        continue;
      for (auto &target : *replacement) {
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
  SmallVector<Attribute> rewritten;
  rewritten.reserve(raw.size());
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
    // Clock and reset have exactly one ground identity each. Rewrite them
    // before copying the event annotation, preserving every other member.
    for (unsigned member = 1; member < members.size(); ++member) {
      if (auto &plan = replacements[index][member]) {
        auto spelling = loweredSpelling(plan->front());
        if (!spelling)
          return failure();
        annotation.setMember(members[member], spelling);
      }
    }
    auto &events = replacements[index][0];
    if (!events) {
      rewritten.push_back(annotation.getAttr());
      continue;
    }
    for (auto &replacement : *events) {
      auto spelling = loweredSpelling(replacement);
      if (!spelling)
        return failure();
      Annotation leaf(annotation.getAttr());
      leaf.setMember("target", spelling);
      rewritten.push_back(leaf.getAttr());
    }
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(module.getContext(),
                                                     rewritten));
  return success();
}
