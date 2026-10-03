// See LICENSE for license details.
#include "goldengate/TriggerWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/SmallVector.h"

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::consumeUnobservedTriggerSources(
    CircuitOp circuit, unsigned &consumed, std::string &error) {
  consumed = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "TriggerWiring needs retained annotations";
    return failure();
  }

  unsigned credits = 0, debits = 0, sinks = 0;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::TriggerSink) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSink)) {
      ++sinks;
      continue;
    }
    if (!annotation.isClass(AnnotationClasses::TriggerSource) &&
        !annotation.isClass(AnnotationClasses::InternalTriggerSource))
      continue;
    auto sourceType = annotation.getMember<BoolAttr>("sourceType");
    if (!sourceType) {
      error = "trigger source has no sourceType";
      return failure();
    }
    sourceType.getValue() ? ++credits : ++debits;
  }
  if ((credits == 0) != (debits == 0)) {
    error = "trigger credits and debits must both be present";
    return failure();
  }
  if (sinks != 0) {
    error = "trigger sink hardware is not yet ported";
    return failure();
  }

  SmallVector<Attribute> retained;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::TriggerSource) ||
        annotation.isClass(AnnotationClasses::InternalTriggerSource)) {
      ++consumed;
      continue;
    }
    retained.push_back(attr);
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(),
                                                   retained));
  return success();
}
