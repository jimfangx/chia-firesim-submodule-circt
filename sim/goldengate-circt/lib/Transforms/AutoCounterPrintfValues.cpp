// See LICENSE for license details.
#include "goldengate/AutoCounterPrintfValues.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include <algorithm>
#include <map>

using namespace circt::firrtl;
using namespace mlir;
namespace {
bool matchesValue(CircuitOp circuit, StringRef target, Value value) {
  std::string error;
  if (auto port = goldengate::resolveAnnotationTarget(circuit, target, error)) {
    auto module = dyn_cast<FModuleOp>(port->module.getOperation());
    return module && port->port && port->fieldID == 0 &&
           module.getBodyBlock()->getArgument(*port->port) == value;
  }
  auto *op = goldengate::resolveInternalAnnotationTarget(circuit, target, error);
  return op && op->getNumResults() == 1 && op->getResult(0) == value;
}
} // namespace

LogicalResult goldengate::synthesizeAutoCounterPrintfValues(
    CircuitOp circuit, ArrayRef<AutoCounterEvent> selected,
    SmallVectorImpl<AutoCounterPrintfValue> &values, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter printf values need retained annotations";
    return failure();
  }
  SmallVector<AutoCounterMode> modes;
  llvm::DenseSet<unsigned> seen;
  for (auto event : selected) {
    auto module = event.module;
    if (!module || module->getParentOp() != circuit.getOperation() ||
        !event.event || !event.clock || !event.reset ||
        event.event.getParentBlock() != module.getBodyBlock() ||
        event.clock.getParentBlock() != module.getBodyBlock() ||
        event.reset.getParentBlock() != module.getBodyBlock() ||
        !isa<ClockType>(event.clock.getType()) || event.label.empty()) {
      error = "AutoCounter printf values need module-scope operands after ExpandWhens: " + event.target;
      return failure();
    }
    auto type = dyn_cast<UIntType>(event.event.getType());
    auto reset = dyn_cast<UIntType>(event.reset.getType());
    if (!type || !type.getWidth() || *type.getWidth() <= 0 ||
        !reset || reset.getWidth() != 1 || event.annotationIndex >= raw.size() ||
        !seen.insert(event.annotationIndex).second) {
      error = "AutoCounter printf values need known positive UInt widths, UInt<1> resets and distinct records: " + event.target;
      return failure();
    }
    Annotation annotation(raw[event.annotationIndex]);
    if ((!annotation.isClass(AnnotationClasses::AutoCounter) &&
         !annotation.isClass(AnnotationClasses::InternalAutoCounter)) ||
        annotation.getMember<StringAttr>("target") != StringAttr::get(circuit.getContext(), event.target) ||
        annotation.getMember<StringAttr>("clock") != StringAttr::get(circuit.getContext(), event.clockTarget) ||
        annotation.getMember<StringAttr>("reset") != StringAttr::get(circuit.getContext(), event.resetTarget) ||
        annotation.getMember<StringAttr>("label") != StringAttr::get(circuit.getContext(), event.label) ||
        !matchesValue(circuit, event.target, event.event) ||
        !matchesValue(circuit, event.clockTarget, event.clock) ||
        !matchesValue(circuit, event.resetTarget, event.reset)) {
      error = "AutoCounter printf selection no longer matches retained targets: " + event.target;
      return failure();
    }
    auto operation = annotation.getMember<DictionaryAttr>("opType");
    auto mode = operation ? operation.getAs<StringAttr>("class") : StringAttr();
    // Missing opType uses the annotation's default Accumulate. Present values
    // must use the serialized PerfCounterOps class, as in the SFC handoff.
    if (!annotation.getDict().get("opType") ||
        (mode && mode.getValue() == AnnotationClasses::AutoCounterAccumulate))
      modes.push_back(AutoCounterMode::Accumulate);
    else if (mode && mode.getValue() == AnnotationClasses::AutoCounterIdentity)
      modes.push_back(AutoCounterMode::Identity);
    else {
      error = "unsupported AutoCounter opType: " + event.target;
      return failure();
    }
  }

  std::map<Operation *, circt::Namespace> namespaces;
  for (auto [index, event] : llvm::enumerate(selected)) {
    auto module = event.module;
    auto [entry, inserted] = namespaces.try_emplace(module.getOperation());
    auto &names = entry->second;
    if (inserted) {
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name"))
          names.newName(name.getValue());
      });
    }
    OpBuilder b(module.getContext());
    b.setInsertionPointToEnd(module.getBodyBlock());
    auto loc = module.getLoc();
    unsigned eventWidth = *cast<UIntType>(event.event.getType()).getWidth();
    bool accumulate = modes[index] == AutoCounterMode::Accumulate;
    // Scala Identity's unknown-width register is constrained by both its
    // event connection and its UInt<64>(0) reset literal (InferWidths).
    unsigned stateWidth = accumulate ? 64 : std::max(64u, eventWidth);
    auto stateType = UIntType::get(b.getContext(), stateWidth);
    auto zero = b.create<ConstantOp>(loc, stateType, llvm::APInt(stateWidth, 0));
    auto state = b.create<RegResetOp>(loc, stateType, event.clock, event.reset,
        zero, names.newName(event.label + (accumulate ? "_counter" : "_reg")));
    Value next = event.event;
    if (accumulate) {
      auto sum = b.create<AddPrimOp>(loc, state.getResult(), event.event);
      next = b.create<NodeOp>(loc, sum.getResult(),
          b.getStringAttr(names.newName(event.label + "_next"))).getResult();
      // SFC's connect truncates the inferred add width to UInt<64>.
      next = b.create<BitsPrimOp>(loc, next, 63, 0);
    } else if (eventWidth < stateWidth) {
      next = b.create<PadPrimOp>(loc, next, stateWidth);
    }
    b.create<StrictConnectOp>(loc, state.getResult(), next);
    Value enable = b.create<NEQPrimOp>(loc, event.event,
        accumulate ? Value(zero.getResult()) : Value(state.getResult()));
    values.push_back({event, modes[index], state.getResult(),
                     accumulate ? state.getResult() : event.event, enable});
  }
  return success();
}

LogicalResult goldengate::synthesizeAutoCounterPrintf(
    CircuitOp circuit, ArrayRef<AutoCounterEvent> selected,
    SmallVectorImpl<AutoCounterPrintf> &prints, std::string &error) {
  SmallVector<AutoCounterPrintfValue> values;
  // All fallible selection/mode/SSA checks precede mutation in this helper.
  if (failed(synthesizeAutoCounterPrintfValues(circuit, selected, values, error)))
    return failure();
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  std::map<Operation *, circt::Namespace> namespaces;
  for (auto value : values) {
    auto module = value.source.module;
    auto [entry, inserted] = namespaces.try_emplace(module.getOperation());
    auto &names = entry->second;
    if (inserted) {
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name"))
          names.newName(name.getValue());
      });
    }
    OpBuilder b(module.getContext());
    b.setInsertionPointToEnd(module.getBodyBlock());
    auto loc = module.getLoc();
    auto bit = UIntType::get(b.getContext(), 1);
    auto trigger = b.create<WireOp>(loc, bit, names.newName("trigger"));
    auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
    b.create<StrictConnectOp>(loc, trigger.getResult(), one.getResult());
    auto enable = b.create<AndPrimOp>(loc, trigger.getResult(), value.printEnable);
    auto ref = StringRef(value.source.target).split('>').second;
    auto print = b.create<PrintFOp>(loc, value.source.clock, enable.getResult(),
        "[AutoCounter] " + value.source.label + ": %d\n",
        ValueRange{value.valueToPrint}, names.newName(ref.str() +
            (value.mode == AutoCounterMode::Accumulate ? "_print" : "_identity_print")));
    auto prefix = "~" + circuit.getName().str() + "|" + module.getName().str() + ">";
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::InternalTriggerSink)),
        b.getNamedAttr("target", b.getStringAttr(prefix + trigger.getName().str())),
        b.getNamedAttr("clock", b.getStringAttr(value.source.clockTarget))}));
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::SynthPrintf)),
        b.getNamedAttr("target", b.getStringAttr(prefix + print.getName().str()))}));
    prints.push_back({value, trigger, print});
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), annotations));
  return success();
}
