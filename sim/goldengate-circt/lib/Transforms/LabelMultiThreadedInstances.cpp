// See LICENSE for license details.
#include "goldengate/LabelMultiThreadedInstances.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/STLExtras.h"

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::labelMultiThreadedInstances(
    CircuitOp circuit, bool enableMultiThreading, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "LabelMultiThreadedInstances needs retained annotations";
    return failure();
  }

  auto *context = circuit.getContext();
  Builder builder(context);
  SmallVector<Attribute> retained, modelLabels;
  auto addModel = [&](Attribute label) {
    if (!llvm::is_contained(modelLabels, label))
      modelLabels.push_back(label);
  };
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::FAMEModel)) {
      addModel(attr);
      continue;
    }
    if (annotation.isClass(AnnotationClasses::EnableModelMultiThreading)) {
      if (enableMultiThreading) {
        auto target = annotation.getMember<StringAttr>("target");
        if (!target) {
          error = "threading annotation has no instance target";
          return failure();
        }
        retained.push_back(attr);
        addModel(DictionaryAttr::get(
            context,
            {builder.getNamedAttr("class", StringAttr::get(
                 context, AnnotationClasses::FAMEModel)),
             builder.getNamedAttr("target", target)}));
      }
      continue;
    }
    retained.push_back(attr);
  }
  retained.append(modelLabels.begin(), modelLabels.end());
  circuit->setAttr("rawAnnotations", ArrayAttr::get(context, retained));
  return success();
}
