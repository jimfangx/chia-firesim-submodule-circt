// See LICENSE for license details.
#include "goldengate/AutoCounterPrintfValues.h"
#include "goldengate/AutoCounterResetGate.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <random>
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
// Interpret the generated FIRRTL graph, including actual register drivers.
// APInt preserves events wider than the accumulator instead of hiding overflow.
struct Interpreter {
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, llvm::APInt> inputs, state, memo;
  Interpreter(FModuleOp module) {
    module.walk([&](StrictConnectOp op) { drivers[op.getDest()] = op.getSrc(); });
    module.walk([&](RegResetOp op) {
      state[op.getResult()] = llvm::APInt(width(op.getResult()), 0);
    });
  }
  llvm::APInt eval(Value value) {
    if (auto found = inputs.find(value); found != inputs.end()) return found->second;
    if (auto found = state.find(value); found != state.end()) return found->second;
    if (auto found = memo.find(value); found != memo.end()) return found->second;
    auto *op = value.getDefiningOp();
    require(op != nullptr, "unbound interpreter input");
    llvm::APInt result(width(value), 0);
    if (auto c = dyn_cast<ConstantOp>(op)) result = c.getValue();
    else if (isa<WireOp>(op)) result = eval(drivers.lookup(value));
    else if (isa<NodeOp, PadPrimOp>(op)) result = eval(op->getOperand(0));
    else if (auto bits = dyn_cast<BitsPrimOp>(op))
      result = eval(bits.getInput()).lshr(bits.getLo());
    else if (auto mux = dyn_cast<MuxPrimOp>(op))
      result = eval(eval(mux.getSel()).isZero() ? mux.getLow() : mux.getHigh());
    else if (isa<AddPrimOp>(op))
      result = eval(op->getOperand(0)).zextOrTrunc(width(value)) +
               eval(op->getOperand(1)).zextOrTrunc(width(value));
    else if (isa<AndPrimOp>(op))
      result = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa<NEQPrimOp>(op)) {
      auto a = eval(op->getOperand(0)), b = eval(op->getOperand(1));
      unsigned w = std::max(a.getBitWidth(), b.getBitWidth());
      result = llvm::APInt(1, a.zextOrTrunc(w) != b.zextOrTrunc(w));
    } else throw std::runtime_error("unexpected FIRRTL operation in interpreter");
    result = result.zextOrTrunc(width(value));
    memo[value] = result;
    return result;
  }
  void edge(Value clock) {
    auto next = state;
    for (auto &entry : state) {
      auto reg = cast<RegResetOp>(entry.first.getDefiningOp());
      if (reg.getClockVal() != clock) continue;
      next[entry.first] = eval(reg.getResetSignal()).isZero()
          ? eval(drivers.lookup(entry.first)) : eval(reg.getResetValue());
    }
    state = std::move(next);
    memo.clear();
  }
};
void checkStructure(ModuleOp root,
                    ArrayRef<goldengate::AutoCounterPrintfValue> values) {
  require(succeeded(verify(root)), "invalid generated FIRRTL");
  std::set<std::pair<Operation *, std::string>> names;
  for (auto &value : values) {
    auto reg = dyn_cast<RegResetOp>(value.state.getDefiningOp());
    auto event = value.source;
    bool accumulate = value.mode == goldengate::AutoCounterMode::Accumulate;
    require(reg && reg.getClockVal() == event.clock &&
        reg.getResetSignal() == event.reset, "register clock/reset identity mismatch");
    require(width(value.state) == (accumulate ? 64u : std::max(64u, width(event.event))),
            "Scala register width constraint mismatch");
    require(names.emplace(event.module.getOperation(), reg.getName().str()).second,
            "counter names collide");
    require(cast<ConstantOp>(reg.getResetValue().getDefiningOp()).getValue().isZero(),
            "reset value is not zero");
    require(value.valueToPrint == (accumulate ? value.state : event.event),
            "old count/current event printf timing mismatch");
    auto enable = dyn_cast<NEQPrimOp>(value.printEnable.getDefiningOp());
    require(enable && enable->getOperand(0) == event.event,
            "printf enable does not compare the selected event");
  }
}
void checkPrints(CircuitOp circuit, ArrayRef<goldengate::AutoCounterPrintf> prints,
                 ArrayAttr original) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == original.size() + 2 * prints.size(), "annotation count mismatch");
  for (unsigned i = 0; i < original.size(); ++i)
    require(raw[i] == original[i], "original annotation payload changed");
  std::set<std::pair<Operation *, std::string>> names;
  for (auto [i, record] : llvm::enumerate(prints)) {
    auto print = record.print;
    auto trigger = record.trigger;
    auto value = record.value;
    auto condition = dyn_cast<AndPrimOp>(print.getCond().getDefiningOp());
    require(condition && condition.getLhs() == trigger.getResult() &&
        condition.getRhs() == value.printEnable && print.getClock() == value.source.clock &&
        print.getSubstitutions().size() == 1 &&
        print.getSubstitutions()[0] == value.valueToPrint &&
        print.getFormatString() == "[AutoCounter] " + value.source.label + ": %d\n",
        "Scala printf operands/format mismatch");
    auto suffix = value.mode == goldengate::AutoCounterMode::Accumulate
        ? "_print" : "_identity_print";
    auto suggested = StringRef(value.source.target).split('>').second.str() + suffix;
    require(print.getName().starts_with(suggested), "printf suggested name mismatch");
    require(names.emplace(value.source.module.getOperation(), print.getName().str()).second &&
        names.emplace(value.source.module.getOperation(), trigger.getName().str()).second,
        "generated printf/trigger names collide");
    auto sink = cast<DictionaryAttr>(raw[original.size() + 2*i]);
    auto anno = cast<DictionaryAttr>(raw[original.size() + 2*i + 1]);
    require(sink.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::InternalTriggerSink &&
        sink.getAs<StringAttr>("clock").getValue() == value.source.clockTarget &&
        anno.getAs<StringAttr>("class").getValue() == goldengate::AnnotationClasses::SynthPrintf,
        "generated annotation class/clock mismatch");
    std::string error;
    require(goldengate::resolveInternalAnnotationTarget(circuit,
        sink.getAs<StringAttr>("target").getValue(), error) == trigger.getOperation(),
        "trigger annotation identity mismatch: " + error);
    require(goldengate::resolveInternalAnnotationTarget(circuit,
        anno.getAs<StringAttr>("target").getValue(), error) == print.getOperation(),
        "printf statement annotation identity mismatch: " + error);
    Interpreter sim(value.source.module);
    require(sim.eval(trigger.getResult()).getBoolValue(), "trigger default is not true");
    for (bool active : {false, true})
      for (bool enabled : {false, true}) {
        sim.inputs[trigger.getResult()] = llvm::APInt(1, active);
        sim.inputs[value.printEnable] = llvm::APInt(1, enabled);
        sim.memo.clear();
        require(sim.eval(print.getCond()).getBoolValue() == (active && enabled),
                "trigger/enable truth table mismatch");
      }
  }
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %otherClock: !firrtl.clock,
          in %reset: !firrtl.uint<1>, in %e1: !firrtl.uint<1>,
          in %e8: !firrtl.uint<8>, in %e64: !firrtl.uint<64>,
          in %e65: !firrtl.uint<65>, in %e129: !firrtl.uint<129>) {
        %counter_counter = firrtl.wire : !firrtl.uint<1>
        %counter_reg = firrtl.wire : !firrtl.uint<1>
        %counter_next = firrtl.wire : !firrtl.uint<1>
        %trigger = firrtl.wire : !firrtl.uint<1>
        %counter_print = firrtl.wire : !firrtl.uint<1>
        %counter_0_identity_print = firrtl.wire : !firrtl.uint<1>
        firrtl.strictconnect %trigger, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_print, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_0_identity_print, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_counter, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_reg, %reset : !firrtl.uint<1>
        firrtl.strictconnect %counter_next, %reset : !firrtl.uint<1>
      }
    }
  })mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto module = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto w : {1, 8, 64, 65, 129})
    for (bool identity : {false, true}) {
      auto mode = identity ? goldengate::AnnotationClasses::AutoCounterIdentity
                           : goldengate::AnnotationClasses::AutoCounterAccumulate;
      annotations.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(identity
              ? goldengate::AnnotationClasses::InternalAutoCounter
              : goldengate::AnnotationClasses::AutoCounter)),
          b.getNamedAttr("target", b.getStringAttr("~Top|Top>e" + std::to_string(w))),
          b.getNamedAttr("clock", b.getStringAttr(identity ? "~Top|Top>otherClock" : "~Top|Top>clock")),
          b.getNamedAttr("reset", b.getStringAttr("~Top|Top>reset")),
          b.getNamedAttr("label", b.getStringAttr("counter")),
          b.getNamedAttr("opType", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(mode))}))}));
    }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  SmallVector<goldengate::AutoCounterEvent> events;
  std::string error;
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, events, error)), error);
  require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit, events, error)), error);
  events.clear();
  require(succeeded(goldengate::analyzeAutoCounterEvents(circuit, events, error)), error);
  SmallVector<goldengate::AutoCounterPrintfValue> values;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  // A bad final mode must not synthesize any of the preceding valid counters.
  auto changed = SmallVector<Attribute>(raw.begin(), raw.end());
  NamedAttrList bad(cast<DictionaryAttr>(changed.back()));
  bad.set("opType", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("bad.Mode"))}));
  changed.back() = bad.getDictionary(&context);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(changed));
  auto before = dump(*root);
  require(failed(goldengate::synthesizeAutoCounterPrintfValues(circuit, events, values, error)) &&
      values.empty() && before == dump(*root), "invalid late mode mutated IR/results");
  circuit->setAttr("rawAnnotations", raw);
  before = dump(*root);
  auto stale = events;
  stale.back().event = module.getBodyBlock()->getArgument(7);
  require(failed(goldengate::synthesizeAutoCounterPrintfValues(circuit, stale, values, error)) &&
      values.empty() && before == dump(*root), "stale SSA operand mutated IR/results");
  stale = events;
  stale.back() = stale.front();
  require(failed(goldengate::synthesizeAutoCounterPrintfValues(circuit, stale, values, error)) &&
      values.empty() && before == dump(*root), "duplicate event mutated IR/results");
  // Exercise the annotation's default Accumulate mode as well as serialized modes.
  changed.assign(raw.begin(), raw.end());
  NamedAttrList defaultMode(cast<DictionaryAttr>(changed.front()));
  defaultMode.erase("opType");
  changed.front() = defaultMode.getDictionary(&context);
  raw = b.getArrayAttr(changed);
  circuit->setAttr("rawAnnotations", raw);
  SmallVector<goldengate::AutoCounterPrintf> prints;
  before = dump(*root);
  require(failed(goldengate::synthesizeAutoCounterPrintf(circuit, stale, prints, error)) &&
      prints.empty() && before == dump(*root), "invalid printf selection mutated IR/results");
  require(succeeded(goldengate::synthesizeAutoCounterPrintf(circuit, events, prints, error)), error);
  for (auto &print : prints) values.push_back(print.value);
  checkPrints(circuit, prints, raw);
  require(prints.front().trigger.getName() != "trigger" &&
      prints.front().print.getName() != "counter_print", "printf reused existing name");
  checkStructure(*root, values);
  for (auto &value : values) {
    auto name = cast<RegResetOp>(value.state.getDefiningOp()).getName();
    require(name != "counter_counter" && name != "counter_reg",
            "generated register reused a pre-existing declaration name");
  }
  module.walk([&](NodeOp node) {
    require(node.getName() != "counter_next",
            "generated add node reused a pre-existing declaration name");
  });
  Interpreter sim(module);
  SmallVector<llvm::APInt> expected;
  for (auto &value : values) expected.emplace_back(width(value.state), 0);
  std::mt19937_64 random(299);
  for (unsigned cycle = 0; cycle < 1500; ++cycle) {
    bool reset = cycle == 0 || cycle % 31 == 0;
    sim.inputs[module.getBodyBlock()->getArgument(2)] = llvm::APInt(1, reset);
    for (unsigned arg = 3; arg < 8; ++arg) {
      auto input = module.getBodyBlock()->getArgument(arg);
      // Stable runs test Identity's change detector; all-ones pairs force
      // accumulator rollover and high bits exercise truncation above 64 bits.
      if (cycle % 5 != 0 && cycle != 0) continue;
      llvm::APInt sample(width(input), random());
      if (width(input) > 64) sample.setBit(width(input) - 1);
      if (cycle % 10 == 0) sample = llvm::APInt::getAllOnes(width(input));
      if (cycle % 15 == 0) sample.clearAllBits();
      sim.inputs[input] = sample;
    }
    sim.memo.clear();
    auto clock = module.getBodyBlock()->getArgument(cycle % 2);
    for (auto [i, value] : llvm::enumerate(values)) {
      unsigned arg = 3 + i / 2;
      auto input = module.getBodyBlock()->getArgument(arg);
      auto event = reset ? llvm::APInt(width(input), 0) : sim.inputs.lookup(input);
      bool accumulate = value.mode == goldengate::AutoCounterMode::Accumulate;
      require(sim.eval(value.valueToPrint) == (accumulate ? expected[i] : event),
              "printf value differs from Scala rule at cycle " + std::to_string(cycle));
      bool enabled = accumulate ? !event.isZero()
          : event.zextOrTrunc(expected[i].getBitWidth()) != expected[i];
      require(sim.eval(value.printEnable).getBoolValue() == enabled,
              "printf enable differs from Scala rule at cycle " + std::to_string(cycle));
      require(sim.eval(prints[i].print.getCond()).getBoolValue() == enabled &&
          sim.eval(prints[i].print.getSubstitutions()[0]) ==
              (accumulate ? expected[i] : event), "printf emission timing mismatch");
      if (value.source.clock == clock)
        expected[i] = reset ? llvm::APInt(expected[i].getBitWidth(), 0)
            : accumulate ? expected[i] + event.zextOrTrunc(64)
                         : event.zextOrTrunc(expected[i].getBitWidth());
    }
    sim.edge(clock);
    for (auto [i, value] : llvm::enumerate(values))
      require(sim.state.lookup(value.state) == expected[i], "register update mismatch");
  }
  require(succeeded(verify(*root)), "invalid printf FIRRTL");
}
void runGolden(MLIRContext &context, StringRef path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "golden candidate parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  PassManager pm(&context);
  pm.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
  require(succeeded(pm.run(*root)), "golden ExpandWhens failed");
  SmallVector<goldengate::AutoCounterEvent> events;
  std::string error;
  require(succeeded(goldengate::analyzeSelectedAutoCounterEvents(
      circuit, {"CSRFile", "CLINT"}, events, error)), error);
  require(events.size() == 372, "golden selected event count differs");
  require(succeeded(goldengate::gateAutoCounterEventsWithReset(circuit, events, error)), error);
  events.clear();
  require(succeeded(goldengate::analyzeSelectedAutoCounterEvents(
      circuit, {"CSRFile", "CLINT"}, events, error)), error);
  auto raw = circuit->getAttr("rawAnnotations");
  SmallVector<goldengate::AutoCounterPrintfValue> values;
  SmallVector<goldengate::AutoCounterPrintf> prints;
  require(succeeded(goldengate::synthesizeAutoCounterPrintf(circuit, events, prints, error)), error);
  for (auto &print : prints) values.push_back(print.value);
  checkStructure(*root, values);
  for (auto &value : values)
    require(value.mode == goldengate::AutoCounterMode::Accumulate, "golden mode mismatch");
  checkPrints(circuit, prints, cast<ArrayAttr>(raw));
  require(succeeded(verify(*root)), "invalid golden printf FIRRTL");
  llvm::outs() << "Checked " << prints.size() << " golden printf operations and annotation pairs\n";
  llvm::outs() << "Checked " << values.size() << " golden candidate printf value graphs\n";
}
} // namespace
int main(int argc, char **argv) {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    run(context);
    if (argc == 2) runGolden(context, argv[1]);
    llvm::outs() << "AutoCounter printf values PASS\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << "AutoCounter printf values FAIL: " << error.what() << '\n';
    return 1;
  }
}
