// See LICENSE for license details.
#include "goldengate/AutoCounterResetGate.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseSet.h"
#include <map>

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::gateAutoCounterEventsWithReset(
    CircuitOp circuit, llvm::ArrayRef<AutoCounterEvent> selected,
    std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "AutoCounter reset gating needs retained annotations";
    return failure();
  }
  llvm::DenseSet<unsigned> seen;
  for (auto event : selected) {
    auto module = event.module;
    // The Scala helper runs on LowForm. A value scoped inside a when cannot
    // feed a gate appended at module scope; reject it before any mutation.
    if (!module || module->getParentOp() != circuit.getOperation() ||
        !event.event || !event.reset || !event.clock ||
        event.event.getParentBlock() != module.getBodyBlock() ||
        event.reset.getParentBlock() != module.getBodyBlock() ||
        event.clock.getParentBlock() != module.getBodyBlock() ||
        !isa<ClockType>(event.clock.getType())) {
      error = "AutoCounter reset gating needs module-scope operands after ExpandWhens: " +
              event.target;
      return failure();
    }
    auto type = dyn_cast<UIntType>(event.event.getType());
    auto resetType = dyn_cast<UIntType>(event.reset.getType());
    if (!type || !type.getWidth() || *type.getWidth() <= 0 || !resetType ||
        resetType.getWidth() != 1 || event.label.empty() ||
        event.annotationIndex >= raw.size() ||
        !seen.insert(event.annotationIndex).second) {
      error = "AutoCounter reset gating needs a known positive UInt event width, "
              "a synchronous UInt<1> reset, a label, and distinct annotation indices: " +
              event.target;
      return failure();
    }
    Annotation annotation(raw[event.annotationIndex]);
    if ((!annotation.isClass(AnnotationClasses::AutoCounter) &&
         !annotation.isClass(AnnotationClasses::InternalAutoCounter)) ||
        annotation.getMember<StringAttr>("target") !=
            StringAttr::get(circuit.getContext(), event.target) ||
        annotation.getMember<StringAttr>("clock") !=
            StringAttr::get(circuit.getContext(), event.clockTarget) ||
        annotation.getMember<StringAttr>("reset") !=
            StringAttr::get(circuit.getContext(), event.resetTarget)) {
      error = "AutoCounter selection no longer matches retained annotation: " + event.target;
      return failure();
    }
  }

  std::map<Operation *, circt::Namespace> namespaces;
  SmallVector<Attribute> updated(raw.begin(), raw.end());
  for (auto event : selected) {
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
    auto name = names.newName(event.label);
    OpBuilder b(module.getContext());
    b.setInsertionPointToEnd(module.getBodyBlock());
    auto loc = module.getLoc();
    auto type = cast<UIntType>(event.event.getType());
    auto wire = b.create<WireOp>(loc, type, b.getStringAttr(name));
    auto zero = b.create<ConstantOp>(loc, type, llvm::APInt(*type.getWidth(), 0));
    auto gated = b.create<MuxPrimOp>(loc, event.reset, zero, event.event);
    b.create<StrictConnectOp>(loc, wire.getResult(), gated.getResult());
    NamedAttrList annotation(cast<DictionaryAttr>(raw[event.annotationIndex]));
    annotation.set("target", b.getStringAttr("~" + circuit.getName().str() +
        "|" + module.getName().str() + ">" + name));
    updated[event.annotationIndex] = annotation.getDictionary(circuit.getContext());
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), updated));
  return success();
}
