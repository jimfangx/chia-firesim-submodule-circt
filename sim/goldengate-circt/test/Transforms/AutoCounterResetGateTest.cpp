// See LICENSE for license details.
#include "goldengate/AutoCounterResetGate.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/APSInt.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
#include <set>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
// Check the new boundary against pre-transform SSA operands and exercise both
// reset states with values at the event's actual width (including >64 bits).
void checkGates(ModuleOp root, CircuitOp circuit,
                ArrayRef<goldengate::AutoCounterEvent> before, ArrayAttr raw) {
  std::string error;
  require(succeeded(verify(root)), "gated IR verification failed");
  SmallVector<goldengate::AutoCounterEvent> after;
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, after, error)), error);
  auto updated = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == updated.size(), "annotation cardinality changed");
  for (auto &event : before) {
    auto found = llvm::find_if(after, [&](auto &candidate) {
      return candidate.annotationIndex == event.annotationIndex;
    });
    require(found != after.end(), "gated annotation no longer resolves");
    auto wire = dyn_cast_or_null<WireOp>(found->event.getDefiningOp());
    require(bool(wire), "gated event is not a wire");
    require(found->clock == event.clock && found->reset == event.reset,
            "clock or reset identity changed");
    StrictConnectOp driver;
    for (auto *user : wire->getUsers())
      if (auto connect = dyn_cast<StrictConnectOp>(user))
        if (connect.getDest() == wire.getResult()) {
          require(!driver, "multiple gate drivers");
          driver = connect;
        }
    require(bool(driver), "missing gate driver");
    auto mux = dyn_cast_or_null<MuxPrimOp>(driver.getSrc().getDefiningOp());
    require(mux && mux.getSel() == event.reset && mux.getLow() == event.event,
            "gate does not mask the original event with its reset");
    auto zero = dyn_cast_or_null<ConstantOp>(mux.getHigh().getDefiningOp());
    require(zero && zero.getValue().isZero() &&
                mux.getType() == event.event.getType(), "zero arm or width changed");
    unsigned width = *cast<UIntType>(event.event.getType()).getWidth();
    for (auto sample : {llvm::APInt(width, 0), llvm::APInt(width, 1),
                        llvm::APInt::getAllOnes(width)})
      for (bool reset : {false, true}) {
        auto result = reset ? zero.getValue() : sample;
        require(result == (reset ? llvm::APInt(width, 0) : sample),
                "reset gating truth table mismatch");
      }
    NamedAttrList expected(cast<DictionaryAttr>(raw[event.annotationIndex]));
    expected.set("target", cast<DictionaryAttr>(updated[event.annotationIndex]).get("target"));
    require(expected.getDictionary(root.getContext()) == updated[event.annotationIndex],
            "AutoCounter payload changed beyond target");
  }
  std::set<unsigned> selected;
  for (auto &event : before) selected.insert(event.annotationIndex);
  for (unsigned i = 0; i < raw.size(); ++i)
    if (!selected.count(i)) require(raw[i] == updated[i], "unselected annotation changed");
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>,
                        in %event: !firrtl.uint<8>, in %wide: !firrtl.uint<129>,
                        in %badReset: !firrtl.uint<2>) {
        %alias = firrtl.node %event : !firrtl.uint<8>
        %counter = firrtl.wire : !firrtl.uint<1>
        %counter_0 = firrtl.wire : !firrtl.uint<1>
        firrtl.strictconnect %counter, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_0, %reset : !firrtl.uint<1>
        firrtl.when %reset : !firrtl.uint<1> {
          %nested = firrtl.node %event : !firrtl.uint<8>
        }
      }
    }
  })mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto record = [&](StringRef event, StringRef klass) {
    return b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(klass)),
        b.getNamedAttr("target", b.getStringAttr("~Top|Top>" + event)),
        b.getNamedAttr("clock", b.getStringAttr("~Top|Top>clock")),
        b.getNamedAttr("reset", b.getStringAttr("~Top|Top>reset")),
        b.getNamedAttr("label", b.getStringAttr("counter")),
        b.getNamedAttr("description", b.getStringAttr("payload survives")),
        b.getNamedAttr("opType", b.getStringAttr("Identity")),
        b.getNamedAttr("coverGenerated", b.getBoolAttr(false))});
  };
  circuit->setAttr("rawAnnotations", b.getArrayAttr({
      record("event", goldengate::AnnotationClasses::AutoCounter),
      b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Unrelated"))}),
      record("alias", goldengate::AnnotationClasses::InternalAutoCounter),
      record("wide", goldengate::AnnotationClasses::AutoCounter)}));
  SmallVector<goldengate::AutoCounterEvent> events;
  std::string error;
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, events, error)) &&
              events.size() == 3, error);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto original = dump(*root);
  auto invalid = events;
  auto module = events.front().module;
  invalid.back().reset = module.getBodyBlock()->getArgument(4);
  require(failed(goldengate::gateAutoCounterEventsWithReset(circuit, invalid, error)) &&
              dump(*root) == original, "invalid late reset mutated IR");
  invalid = events;
  invalid.back().annotationIndex = invalid.front().annotationIndex;
  require(failed(goldengate::gateAutoCounterEventsWithReset(circuit, invalid, error)) &&
              dump(*root) == original, "duplicate selection mutated IR");
  // Only two of the three valid records were selected. Leave the wide event's
  // annotation and hardware untouched, as the Scala helper does.
  require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit,
                         ArrayRef(events).take_front(2), error)), error);
  checkGates(*root, circuit, ArrayRef(events).take_front(2), raw);
  SmallVector<goldengate::AutoCounterEvent> updated;
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, updated, error)), error);
  require(updated[0].target != updated[1].target &&
              updated[0].target != "~Top|Top>counter" &&
              updated[0].target != "~Top|Top>counter_0", "gate label collided");
  // The recorded Rocket fixture has named events inside whens. Check the
  // helper's LowForm precondition, then the actual prerequisite that fixes it.
  auto nestedRecord = record("nested", goldengate::AnnotationClasses::AutoCounter);
  auto previous = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> withNested(previous.begin(), previous.end());
  withNested.push_back(nestedRecord);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(withNested));
  SmallVector<goldengate::AutoCounterEvent> nestedEvents;
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, nestedEvents, error)), error);
  original = dump(*root);
  require(failed(goldengate::gateAutoCounterEventsWithReset(circuit,
                         {nestedEvents.back()}, error)) && dump(*root) == original,
          "when-scoped event mutated IR before rejection");
  PassManager lowForm(&context);
  lowForm.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
  require(succeeded(lowForm.run(*root)), "fixture ExpandWhens failed");
  nestedEvents.clear();
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, nestedEvents, error)), error);
  raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit,
                         {nestedEvents.back()}, error)), error);
  checkGates(*root, circuit, {nestedEvents.back()}, raw);
  auto wide = nestedEvents[2];
  raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit, {wide}, error)), error);
  checkGates(*root, circuit, {wide}, raw);
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    if (argc == 2) {
      auto root = parseSourceFile<ModuleOp>(argv[1], &context);
      require(bool(root), "golden candidate MLIR parse failed");
      auto circuit = *root->getOps<CircuitOp>().begin();
      PassManager lowForm(&context);
      lowForm.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
      require(succeeded(lowForm.run(*root)), "golden candidate ExpandWhens failed");
      SmallVector<goldengate::AutoCounterEvent> events;
      std::string error;
      require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, events, error)), error);
      auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
      require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit, events, error)), error);
      checkGates(*root, circuit, events, raw);
      llvm::outs() << "Checked " << events.size() << " golden candidate reset gates\n";
    }
    llvm::outs() << "AutoCounter reset gating: PASS\n";
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n';
    return 1;
  }
  return 0;
}
