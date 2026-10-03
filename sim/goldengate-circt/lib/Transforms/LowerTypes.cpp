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

using namespace circt::firrtl;
using namespace mlir;

namespace {
// SFC DestructTypes records each aggregate reference as referring to all of
// its ground children. Walk the selected CIRCT subtype in declaration order,
// including vector indices, to apply that one-to-many rename to raw metadata.
struct GroundPortTarget {
  FModuleLike module;
  unsigned port;
  uint64_t fieldID;
  StringAttr symbol;
};

void collectGroundPorts(FIRRTLBaseType type, FModuleLike module, unsigned port,
                        uint64_t fieldID,
                        SmallVectorImpl<GroundPortTarget> &targets) {
  if (auto bundle = dyn_cast<BundleType>(type)) {
    for (auto [i, element] : llvm::enumerate(bundle.getElements()))
      collectGroundPorts(element.type, module, port,
                         fieldID + bundle.getFieldID(i), targets);
  } else if (auto vector = dyn_cast<FVectorType>(type)) {
    for (unsigned i = 0; i < vector.getNumElements(); ++i)
      collectGroundPorts(vector.getElementType(), module, port,
                         fieldID + vector.getFieldID(i), targets);
  } else {
    targets.push_back({module, port, fieldID, {}});
  }
}
// LowerTypes can leave duplicate portNames even though its printed SSA names
// are distinct. Reserve all existing spellings, then rename duplicate ports in
// declaration order using CIRCT's namespace. Instances bind by result index;
// update their portNames as well so exported FIRRTL connects the same leaves.
void uniquifyLoweredPortNames(CircuitOp circuit) {
  llvm::DenseMap<StringAttr, ArrayAttr> renamed;
  for (auto owner : circuit.getOps<FModuleLike>()) {
    circt::Namespace names;
    llvm::DenseSet<Attribute> seen;
    auto original = owner.getPortNamesAttr();
    for (auto attr : original)
      if (seen.insert(attr).second)
        names.newName(cast<StringAttr>(attr).getValue());
    seen.clear();
    SmallVector<Attribute> ports;
    for (auto attr : original)
      ports.push_back(seen.insert(attr).second ? attr :
          StringAttr::get(circuit.getContext(),
                         names.newName(cast<StringAttr>(attr).getValue())));
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
  // an absent plan leaves unrelated or internal ground targets untouched.
  SmallVector<std::optional<SmallVector<GroundPortTarget>>> replacements(raw.size());
  for (auto [index, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    const bool dontTouch = annotation.isClass(AnnotationClasses::DontTouch);
    const bool autoCounter = annotation.isClass(AnnotationClasses::AutoCounter);
    if (!dontTouch && !autoCounter)
      continue;
    auto spelling = annotation.getMember<StringAttr>("target");
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
      if (autoCounter && !type.isGround()) {
        error = "AutoCounter event target must select a ground value: " +
                spelling.getValue().str();
        return failure();
      }
      replacements[index].emplace();
      collectGroundPorts(type, target->module, *target->port, *target->fieldID,
                         *replacements[index]);
      continue;
    }
    // Preserve the handoff's internal ground AutoCounter events. Port events
    // above follow namespace renames just like selected aggregate leaves.
    if (autoCounter && !local.contains('.') && !local.contains('['))
      continue;
    if (autoCounter ||
        !resolveInternalAnnotationTarget(circuit, spelling.getValue(),
                                         resolutionError)) {
      error = "unresolved retained annotation target " +
              spelling.getValue().str() + ": " + resolutionError;
      return failure();
    }
  }

  // Materialize identities only after all retained selectors pass preflight.
  // Reuse existing leaf symbols, including their visibility and InnerRef users.
  // Temporary private leaf symbols carry annotations through CIRCT expansion
  // and namespace uniquification. Aggregate-root symbols would not survive
  // lowering, so never create them.
  circt::hw::InnerSymbolNamespaceCollection namespaces;
  llvm::DenseMap<Operation *, SmallVector<StringAttr>> temporarySymbols;
  for (auto &replacement : replacements) {
    if (!replacement)
      continue;
    for (auto &target : *replacement) {
      auto symbols = target.module.getPortSymbolAttr(target.port);
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
      target.module.setPortSymbolsAttr(target.port,
          circt::hw::InnerSymAttr::get(module.getContext(), properties));
      temporarySymbols[target.module].push_back(target.symbol);
    }
  }
  // Remove only identities introduced here, also on a downstream pass failure.
  // Existing symbol properties and native InnerRefs remain untouched.
  auto cleanup = llvm::make_scope_exit([&] {
    for (auto &[op, names] : temporarySymbols) {
      auto owner = cast<FModuleLike>(op);
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

  uniquifyLoweredPortNames(circuit);
  circt::hw::InnerSymbolTableCollection tables;
  SmallVector<Attribute> rewritten;
  rewritten.reserve(raw.size());
  for (auto [index, attr] : llvm::enumerate(raw)) {
    if (!replacements[index]) {
      rewritten.push_back(attr);
      continue;
    }
    for (auto &replacement : *replacements[index]) {
      auto lowered = tables.getInnerSymbolTable(replacement.module)
                         .lookup(replacement.symbol);
      if (!lowered || !lowered.isPort() || lowered.getField() != 0 ||
          !cast<FIRRTLBaseType>(replacement.module.getPortType(lowered.getPort())).isGround()) {
        error = "LowerTypes did not preserve annotated ground port identity " +
                replacement.symbol.getValue().str();
        return failure();
      }
      auto spelling = "~" + circuitName + "|" +
          replacement.module.getModuleName().str() + ">" +
          replacement.module.getPortName(lowered.getPort()).str();
      Annotation annotation(attr);
      annotation.setMember("target", StringAttr::get(module.getContext(), spelling));
      rewritten.push_back(annotation.getAttr());
    }
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(module.getContext(),
                                                     rewritten));
  return success();
}
