// See LICENSE for license details.
#include "goldengate/LowerTypes.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/Pass/PassManager.h"

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::lowerTypesWithRetainedTargets(
    ModuleOp module, CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "LowerTypes needs retained annotations";
    return failure();
  }
  const std::string circuitName = circuit.getName().str();
  SmallVector<std::string> replacements(raw.size());
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
      replacements[index] = "~" + circuitName + "|" +
                            target->module.getModuleName().str() + ">" +
                            target->groundPortName;
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
    if (replacements[index].empty()) {
      rewritten.push_back(attr);
      continue;
    }
    auto spelling = llvm::StringRef(replacements[index]);
    std::string resolutionError;
    auto lowered = resolveAnnotationTarget(circuit, spelling, resolutionError);
    if (!lowered || !lowered->port || *lowered->fieldID != 0) {
      error = "LowerTypes did not create annotated ground port " +
              spelling.str() + ": " + resolutionError;
      return failure();
    }
    Annotation annotation(attr);
    annotation.setMember("target", StringAttr::get(module.getContext(),
                                                    spelling));
    rewritten.push_back(annotation.getAttr());
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(module.getContext(),
                                                     rewritten));
  return success();
}
