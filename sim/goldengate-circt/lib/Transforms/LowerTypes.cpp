// See LICENSE for license details.
#include "goldengate/LowerTypes.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/StringMap.h"

using namespace circt::firrtl;
using namespace mlir;

namespace {
// SFC DestructTypes records each aggregate reference as referring to all of
// its ground children. Walk the selected CIRCT subtype in declaration order,
// including vector indices, to apply that one-to-many rename to raw metadata.
void collectGroundPorts(FIRRTLBaseType type, const std::string &name,
                        SmallVectorImpl<std::string> &names) {
  if (auto bundle = dyn_cast<BundleType>(type)) {
    for (auto element : bundle.getElements())
      collectGroundPorts(element.type, name + "_" + element.name.str(), names);
  } else if (auto vector = dyn_cast<FVectorType>(type)) {
    for (unsigned i = 0; i < vector.getNumElements(); ++i)
      collectGroundPorts(vector.getElementType(), name + "_" + std::to_string(i),
                         names);
  } else {
    names.push_back(name);
  }
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
  SmallVector<std::optional<SmallVector<std::string>>> replacements(raw.size());
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
    // Ground AutoCounter references do not change spelling through LowerTypes.
    // The SFC handoff contains one aggregate event on PTW.io.mem.s2_nack.
    auto local = spelling.getValue().split('>').second;
    if (autoCounter && !local.contains('.') && !local.contains('['))
      continue;
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
      SmallVector<std::string> names;
      collectGroundPorts(type, target->groundPortName, names);
      // LowerTypes may uniquify colliding port spellings. Until retained
      // targets follow that namespace rename, reject ambiguity before lowering
      // rather than accidentally binding the annotation to another source port.
      llvm::StringMap<unsigned> portNames;
      for (const auto &port : target->module.getPorts()) {
        SmallVector<std::string> leaves;
        if (auto portType = dyn_cast<FIRRTLBaseType>(port.type))
          collectGroundPorts(portType, port.name.str(), leaves);
        else
          leaves.push_back(port.name.str());
        for (const auto &leaf : leaves)
          ++portNames[leaf];
      }
      for (const auto &name : names) {
        if (portNames.lookup(name) != 1) {
          error = "retained target needs a LowerTypes namespace rename: " +
                  spelling.getValue().str();
          return failure();
        }
        replacements[index]->push_back("~" + circuitName + "|" +
            target->module.getModuleName().str() + ">" + name);
      }
      continue;
    }
    if (autoCounter ||
        !resolveInternalAnnotationTarget(circuit, spelling.getValue(),
                                         resolutionError)) {
      error = "unresolved retained annotation target " +
              spelling.getValue().str() + ": " + resolutionError;
      return failure();
    }
  }

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

  SmallVector<Attribute> rewritten;
  rewritten.reserve(raw.size());
  for (auto [index, attr] : llvm::enumerate(raw)) {
    if (!replacements[index]) {
      rewritten.push_back(attr);
      continue;
    }
    for (const auto &replacement : *replacements[index]) {
      auto spelling = llvm::StringRef(replacement);
      std::string resolutionError;
      auto lowered = resolveAnnotationTarget(circuit, spelling, resolutionError);
      if (!lowered || !lowered->port || *lowered->fieldID != 0 ||
          !cast<FIRRTLBaseType>(lowered->module.getPortType(*lowered->port)).isGround()) {
        error = "LowerTypes did not create annotated ground port " +
                spelling.str() + ": " + resolutionError;
        return failure();
      }
      Annotation annotation(attr);
      annotation.setMember("target", StringAttr::get(module.getContext(), spelling));
      rewritten.push_back(annotation.getAttr());
    }
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(module.getContext(),
                                                     rewritten));
  return success();
}
