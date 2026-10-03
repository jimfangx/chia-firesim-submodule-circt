// See LICENSE for license details.
#include "goldengate/AutoCounterAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct ResolvedValue {
  FModuleOp module;
  Value value;
};

std::optional<ResolvedValue> resolveValue(CircuitOp circuit,
                                          llvm::StringRef spelling,
                                          std::string &error) {
  std::string portError;
  if (auto target = goldengate::resolveAnnotationTarget(circuit, spelling,
                                                        portError)) {
    if (!target->port || !target->fieldID || *target->fieldID != 0) {
      error = "target must name a ground port: " + spelling.str();
      return std::nullopt;
    }
    auto module = mlir::dyn_cast<FModuleOp>(target->module.getOperation());
    if (!module) {
      error = "target port belongs to an external module: " + spelling.str();
      return std::nullopt;
    }
    return ResolvedValue{module,
                         module.getBodyBlock()->getArgument(*target->port)};
  }
  std::string internalError;
  auto *op = goldengate::resolveInternalAnnotationTarget(
      circuit, spelling, internalError);
  if (!op || op->getNumResults() != 1) {
    error = "cannot resolve FIRRTL value " + spelling.str() + ": " +
            internalError;
    return std::nullopt;
  }
  return ResolvedValue{op->getParentOfType<FModuleOp>(), op->getResult(0)};
}
} // namespace

LogicalResult goldengate::analyzeAutoCounterEvents(
    CircuitOp circuit, llvm::SmallVectorImpl<AutoCounterEvent> &events,
    std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter analysis needs retained annotations";
    return failure();
  }
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::AutoCounter))
      continue;
    auto target = annotation.getMember<StringAttr>("target");
    auto clock = annotation.getMember<StringAttr>("clock");
    auto reset = annotation.getMember<StringAttr>("reset");
    auto label = annotation.getMember<StringAttr>("label");
    if (!target || !clock || !reset || !label) {
      error = "AutoCounter annotation lacks target, clock, reset, or label";
      return failure();
    }
    auto eventValue = resolveValue(circuit, target.getValue(), error);
    if (!eventValue)
      return failure();
    auto clockValue = resolveValue(circuit, clock.getValue(), error);
    if (!clockValue)
      return failure();
    auto resetValue = resolveValue(circuit, reset.getValue(), error);
    if (!resetValue)
      return failure();
    if (eventValue->module != clockValue->module ||
        eventValue->module != resetValue->module) {
      error = "AutoCounter event, clock, and reset cross module boundaries: " +
              target.getValue().str();
      return failure();
    }
    if (!isa<UIntType>(eventValue->value.getType()) ||
        !isa<ClockType>(clockValue->value.getType()) ||
        !isa<UIntType, ResetType, AsyncResetType>(
            resetValue->value.getType())) {
      error = "AutoCounter event, clock, or reset has an invalid FIRRTL type: " +
              target.getValue().str();
      return failure();
    }
    events.push_back(AutoCounterEvent{
        eventValue->module, eventValue->value, clockValue->value,
        resetValue->value, target.getValue().str(), clock.getValue().str(),
        reset.getValue().str(), label.getValue().str()});
  }
  return success();
}

LogicalResult goldengate::dropDisabledAutoCounterAnnotations(
    CircuitOp circuit, unsigned &removed, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter cleanup needs retained annotations";
    return failure();
  }
  SmallVector<Attribute> retained;
  retained.reserve(raw.size());
  removed = 0;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::AutoCounter) ||
        annotation.isClass(AnnotationClasses::InternalAutoCounter) ||
        annotation.isClass(AnnotationClasses::AutoCounterCoverModule)) {
      ++removed;
      continue;
    }
    retained.push_back(attr);
  }
  circuit->setAttr("rawAnnotations",
                   ArrayAttr::get(circuit.getContext(), retained));
  return success();
}
