// See LICENSE for license details.
#include "goldengate/AutoCounterAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "mlir/IR/SymbolTable.h"

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
LogicalResult analyzeEvents(
    CircuitOp circuit,
    const llvm::SmallPtrSetImpl<Operation *> *coverModules,
    llvm::SmallVectorImpl<goldengate::AutoCounterEvent> &events,
    std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter analysis needs retained annotations";
    return failure();
  }
  SmallVector<goldengate::AutoCounterEvent> resolvedEvents;
  for (auto [index, attr] : llvm::enumerate(raw)) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::AutoCounter) &&
        !annotation.isClass(goldengate::AnnotationClasses::InternalAutoCounter))
      continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (coverModules) {
      auto generated = annotation.getMember<BoolAttr>("coverGenerated");
      if (annotation.getDict().get("coverGenerated") && !generated) {
        error = "AutoCounter coverGenerated must be a boolean";
        return failure();
      }
      // Missing coverGenerated uses the Scala annotation's default false.
      if (generated && generated.getValue()) {
        if (!target || !target.getValue().contains('>')) {
          error = "generated AutoCounter lacks an event reference target";
          return failure();
        }
        // Resolve only the module identity here. Unselected generated events
        // need not resolve their event, clock, reset, or label operands.
        auto enclosing = goldengate::resolveAnnotationTarget(
            circuit, target.getValue().split('>').first, error);
        if (!enclosing)
          return failure();
        if (!coverModules->contains(enclosing->module.getOperation()))
          continue;
      }
    }
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
    resolvedEvents.push_back(goldengate::AutoCounterEvent{
        eventValue->module, eventValue->value, clockValue->value,
        resetValue->value, target.getValue().str(), clock.getValue().str(),
        reset.getValue().str(), label.getValue().str(),
        static_cast<unsigned>(index)});
  }
  events.append(resolvedEvents.begin(), resolvedEvents.end());
  return success();
}
} // namespace

LogicalResult goldengate::analyzeAutoCounterEvents(
    CircuitOp circuit, llvm::SmallVectorImpl<AutoCounterEvent> &events,
    std::string &error) {
  return analyzeEvents(circuit, nullptr, events, error);
}

LogicalResult goldengate::analyzeSelectedAutoCounterEvents(
    CircuitOp circuit, llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    llvm::SmallVectorImpl<AutoCounterEvent> &events, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter selection needs retained annotations";
    return failure();
  }
  llvm::SmallPtrSet<Operation *, 8> coverModules;
  for (auto name : coverModuleNames)
    // File entries are exact module names. Unknown names simply match no
    // event, as in Scala; do not trim whitespace or treat entries as patterns.
    if (auto *op = SymbolTable::lookupSymbolIn(circuit, name))
      if (isa<FModuleLike>(op))
        coverModules.insert(op);
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::AutoCounterCoverModule))
      continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) {
      error = "AutoCounter cover-module annotation lacks a target";
      return failure();
    }
    auto resolved = resolveAnnotationTarget(circuit, target.getValue(), error);
    if (!resolved || resolved->port) {
      if (resolved)
        error = "AutoCounter cover-module target must name a module";
      return failure();
    }
    coverModules.insert(resolved->module.getOperation());
  }
  return analyzeEvents(circuit, &coverModules, events, error);
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
