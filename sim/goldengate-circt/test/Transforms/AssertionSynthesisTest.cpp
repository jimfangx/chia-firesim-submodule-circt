// See LICENSE for license details.
#include "goldengate/AssertionSynthesis.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string text(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  throw std::runtime_error("missing module " + name.str());
}
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  Fixture(MLIRContext &ctx, StringRef base) {
    llvm::SourceMgr manager;
    for (auto path : {base.str() + ".fir", base.str() + ".anno.json"}) {
      auto file = llvm::MemoryBuffer::getFile(path); require(bool(file), "missing " + path);
      manager.AddNewSourceBuffer(std::move(*file), llvm::SMLoc());
    }
    DefaultTimingManager timer; auto scope = timer.getRootScope();
    FIRParserOptions options; options.numAnnotationFiles = 1;
    root = importFIRFile(manager, &ctx, scope, options); require(bool(root), "fixture parse failed");
    circuit = *root->getOps<CircuitOp>().begin();
    if (!circuit->hasAttr("rawAnnotations")) circuit->setAttr("rawAnnotations", ArrayAttr::get(&ctx, {}));
  }
};
Value driver(FModuleOp module, Value destination) {
  Value found;
  module.walk([&](Operation *op) { if (auto connect = dyn_cast<FConnectLike>(op))
    if (connect.getDest() == destination) { require(!found, "multiple drivers"); found = connect.getSrc(); } });
  require(bool(found), "missing driver"); return found;
}
void lowBits(Value value, SmallVectorImpl<Value> &bits) {
  if (auto cat = value.getDefiningOp<CatPrimOp>()) {
    lowBits(cat->getOperand(1), bits); lowBits(cat->getOperand(0), bits);
  } else bits.push_back(value);
}
void exclusion(Fixture &f, bool dict) {
  Builder b(f.circuit.getContext());
  Attribute target = b.getArrayAttr({b.getStringAttr("Top"), b.getStringAttr("right")});
  if (dict) target = b.getDictionaryAttr({b.getNamedAttr("_1", b.getStringAttr("Top")), b.getNamedAttr("_2", b.getStringAttr("right"))});
  f.circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("midas.targetutils.ExcludeInstanceAssertsAnnotation")),
    b.getNamedAttr("target", target)})}));
}
void positive(MLIRContext &ctx, StringRef path, unsigned excludeMode) {
  Fixture f(ctx, path); if (excludeMode) exclusion(f, excludeMode == 2);
  unsigned count = 999; std::string error;
  require(succeeded(goldengate::synthesizeAssertions(f.circuit, count, error)), error);
  require(count == (excludeMode ? 4 : 7) && succeeded(verify(*f.root)), "synthesized count/verification");
  unsigned assertions = 0, stops = 0, prints = 0;
  f.circuit.walk([&](Operation *op) {
    assertions += isa<AssertOp>(op); prints += isa<PrintFOp>(op);
    if (auto stop = dyn_cast<StopOp>(op)) { ++stops; require(stop.getExitCode() == 0, "failing stop survived"); }
  });
  require(assertions == 0 && stops == 1 && prints == 0, "legacy/native statement replacement");
  auto top = named(f.circuit, "Top");
  require(top.getNumPorts() == 5 + 3 * (excludeMode ? 1 : 2), "top must export only grouped domain assertion/reset/clock ports");
  for (unsigned i = 5; i < top.getNumPorts(); ++i)
    require(top.getPortName(i).take_front(12) == "midasAsserts", "unbound intermediate scalar assertion output leaked to top");
  std::map<std::string, DictionaryAttr> channels;
  std::set<std::string> resetSinks;
  SmallVector<DictionaryAttr> bridges;
  for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto anno = cast<DictionaryAttr>(attr); auto cls = anno.getAs<StringAttr>("class");
    if (cls == goldengate::AnnotationClasses::BridgeIO) bridges.push_back(anno);
    if (cls == goldengate::AnnotationClasses::ChannelConnection)
      require(channels.emplace(anno.getAs<StringAttr>("globalName").getValue().str(), anno).second, "duplicate source channel");
    if (cls == goldengate::AnnotationClasses::GlobalResetSink)
      resetSinks.insert(anno.getAs<StringAttr>("target").getValue().str());
  }
  require(bridges.size() == (excludeMode ? 1 : 2) && channels.size() == 2 * bridges.size() && resetSinks.size() == bridges.size(), "bridge/channel/reset inventory");
  unsigned messagesSeen = 0;
  for (auto bridge : bridges) {
    require(bridge.getAs<StringAttr>("widgetClass") == "midas.widgets.AssertBridgeModule", "wrong widget class");
    auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
    auto asserts = key.getAs<StringAttr>("assertPortName").getValue();
    auto reset = key.getAs<StringAttr>("resetPortName").getValue();
    auto messages = key.getAs<ArrayAttr>("assertMessages");
    require(resetSinks.count("~Top|Top>" + reset.str()), "global-reset sink target mismatch");
    auto arg = [&](StringRef name) -> Value { for (unsigned i = 0; i < top.getNumPorts(); ++i) if (top.getPortName(i) == name) return top.getArgument(i); throw std::runtime_error("missing generated port"); };
    auto resetValue = driver(top, arg(reset)).getDefiningOp<ConstantOp>();
    require(resetValue && resetValue.getValue().isZero(), "global reset default must be false");
    auto channel = channels.at(asserts.str());
    require(channels.at(reset.str()).getAs<StringAttr>("clock") == channel.getAs<StringAttr>("clock"), "assert/reset clocks differ");
    auto clockTarget = channel.getAs<StringAttr>("clock").getValue();
    auto clockName = clockTarget.split('>').second;
    auto sourceClock = driver(top, arg(clockName));
    require(sourceClock == top.getArgument(0) || sourceClock == top.getArgument(1), "domain clock root");
    SmallVector<Value> bits; lowBits(driver(top, arg(asserts)), bits);
    require(bits.size() == messages.size(), "packed vector width/message count mismatch");
    for (auto [index, attr] : llvm::enumerate(messages)) {
      auto message = cast<StringAttr>(attr).getValue(); ++messagesSeen;
      Value source = bits[index];
      if (auto arg = dyn_cast<BlockArgument>(source); arg && arg.getOwner() == top.getBodyBlock())
        source = driver(top, source);
      auto module = top;
      if (auto result = dyn_cast<OpResult>(source)) if (auto instance = dyn_cast<InstanceOp>(result.getOwner())) {
        require(message.contains("path: Top." + instance.getName().str() + "]"), "message has wrong instance path for packed bit");
        require(!excludeMode || instance.getName() != "right", "excluded instance exported");
        require(sourceClock == top.getArgument(instance.getName() == "left" ? 0 : 1), "child assertion clock root mismatch");
        module = named(f.circuit, instance.getModuleName());
        source = driver(module, module.getArgument(result.getResultNumber()));
      } else require(message.contains("path: Top]"), "local assertion path mismatch");
      auto node = source.getDefiningOp<NodeOp>(); require(bool(node), "assertion failure must be named");
      auto failure = node->getOperand(0);
      if (message.contains("stop(7)")) require(failure == module.getArgument(3), "fallback stop enable mismatch");
      else if (message.contains("legacy assertion")) require(failure == module.getArgument(2), "legacy stop enable mismatch");
      else {
        require(message.contains(module == top ? "top assertion" : "leaf assertion"), "native message mismatch");
        auto andOp = failure.getDefiningOp<AndPrimOp>(); require(bool(andOp), "native assert missing enable gate");
        auto notOp = andOp->getOperand(1).getDefiningOp<NotPrimOp>(); require(bool(notOp), "native assert missing predicate inversion");
        require(andOp->getOperand(0) == module.getArgument(module == top ? 3 : 2) && notOp->getOperand(0) == module.getArgument(module == top ? 2 : 1), "native assertion predicate/enable mismatch");
      }
    }
  }
  require(messagesSeen == count, "each synthesized assertion needs exactly one diagnostic");
}
void undriven(MLIRContext &ctx, StringRef path) {
  Fixture f(ctx, path); auto leaf = named(f.circuit, "Leaf");
  OpBuilder b(&ctx); b.setInsertionPointToStart(leaf.getBodyBlock());
  auto clock = b.create<WireOp>(leaf.getLoc(), ClockType::get(&ctx), b.getStringAttr("undrivenClock"));
  for (auto assertion : leaf.getOps<AssertOp>()) assertion->setOperand(0, clock.getResult());
  unsigned count = 999; std::string error;
  require(succeeded(goldengate::synthesizeAssertions(f.circuit, count, error)), error);
  require(count == 5 && succeeded(verify(*f.root)), "undriven local assertion must be omitted at both instance paths");
  unsigned top = 0, leftLegacy = 0, rightLegacy = 0, fallback = 0, bridges = 0;
  for (auto attr : f.circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto anno = cast<DictionaryAttr>(attr);
    if (anno.getAs<StringAttr>("class") != goldengate::AnnotationClasses::BridgeIO) continue;
    ++bridges;
    for (auto message : anno.getAs<DictionaryAttr>("widgetConstructorKey").getAs<ArrayAttr>("assertMessages")) {
      auto value = cast<StringAttr>(message).getValue();
      require(!value.contains("leaf assertion"), "undriven assertion diagnostic survived");
      top += value.contains("top assertion");
      leftLegacy += value.contains("path: Top.left]") && value.contains("legacy assertion");
      rightLegacy += value.contains("path: Top.right]") && value.contains("legacy assertion");
      fallback += value.contains("stop(7)");
    }
  }
  require(bridges == 2 && top == 1 && leftLegacy == 1 && rightLegacy == 1 && fallback == 2,
          "omitting an early route must preserve subsequent route indices/messages and domains");
}
void negatives(MLIRContext &ctx, StringRef path) {
  auto reject = [&](const std::function<void(Fixture &)> &change) {
    Fixture f(ctx, path); change(f); auto before = text(*f.root); auto originalArg = named(f.circuit, "Top").getArgument(0);
    unsigned count = 999; std::string error;
    require(failed(goldengate::synthesizeAssertions(f.circuit, count, error)) && !error.empty(), "malformed case accepted");
    require(count == 999 && text(*f.root) == before && named(f.circuit, "Top").getArgument(0) == originalArg, "rejected synthesis mutated original IR/count/SSA");
  };
  reject([&](Fixture &f) {
    auto top = named(f.circuit, "Top"); OpBuilder b(&ctx); b.setInsertionPointToStart(top.getBodyBlock());
    auto clock = b.create<WireOp>(top.getLoc(), ClockType::get(&ctx), b.getStringAttr("ambiguousClock"));
    b.create<ConnectOp>(top.getLoc(), clock.getResult(), top.getArgument(0));
    b.create<ConnectOp>(top.getLoc(), clock.getResult(), top.getArgument(1));
    for (auto assertion : top.getOps<AssertOp>()) assertion->setOperand(0, clock.getResult());
  });
  reject([&](Fixture &f) {
    Builder b(&ctx); f.circuit->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("midas.targetutils.ExcludeInstanceAssertsAnnotation")), b.getNamedAttr("target", b.getStringAttr("not-a-tuple"))})}));
  });
}
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage fixture-basename"); MLIRContext ctx;
    ctx.getOrLoadDialect<FIRRTLDialect>(); ctx.getOrLoadDialect<circt::hw::HWDialect>();
    positive(ctx, argv[1], 0); positive(ctx, argv[1], 1); positive(ctx, argv[1], 2); undriven(ctx, argv[1]); negatives(ctx, argv[1]);
    llvm::outs() << "PASS native/legacy assertions, packed diagnostic correspondence, repeated instances, multiple clocks, exclusions, reset sinks, undriven omission and atomic ambiguous-clock rejection\n";
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
