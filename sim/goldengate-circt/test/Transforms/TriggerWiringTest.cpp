// See LICENSE for license details.
#include "goldengate/TriggerWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
using A = goldengate::AnnotationClasses;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) {
  std::string s; llvm::raw_string_ostream out(s); op->print(out); return s;
}
void run(MLIRContext &context, bool internal, bool mask, StringRef output) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>,
        in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>,
        in %otherClock: !firrtl.clock, out %enabled: !firrtl.uint<1>) {}
    }
  })mlir", &context);
  require(bool(root), "parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context); b.setInsertionPointToEnd(top.getBodyBlock());
  auto bit = UIntType::get(&context, 1);
  auto zero = b.create<ConstantOp>(top.getLoc(), bit, llvm::APInt(1, 0));
  auto sinkNode = b.create<NodeOp>(top.getLoc(), zero.getResult(), b.getStringAttr("trigger"));
  b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(5), sinkNode.getResult());
  auto reference = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  auto source = [&](bool credit, StringRef clock) {
    NamedAttrList attrs;
    attrs.set("class", b.getStringAttr(internal ? A::InternalTriggerSource : A::TriggerSource));
    attrs.set("target", reference(credit ? "credit" : "debit"));
    attrs.set("clock", reference(clock));
    attrs.set("sourceType", b.getBoolAttr(credit));
    if (mask) attrs.set("reset", reference("reset"));
    return attrs.getDictionary(&context);
  };
  auto sink = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(
      internal ? A::InternalTriggerSink : A::TriggerSink)),
      b.getNamedAttr("target", reference("trigger")), b.getNamedAttr("clock", reference("clock"))});
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({reference("clock")}))});
  auto keep = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("example.Keep"))});
  std::string error; unsigned consumed = 99;
  auto set = [&](ArrayRef<Attribute> attrs) { circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs)); };
  auto reject = [&](ArrayRef<Attribute> attrs, StringRef reason) {
    set(attrs); auto before = dump(root.get());
    require(failed(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 0 &&
      dump(root.get()) == before && StringRef(error).contains(reason), "non-atomic rejection: " + error);
  };
  reject({source(true, "clock"), sink}, "both");
  reject({source(true, "clock"), source(true, "clock"), source(false, "clock"), sink}, "one credit");
  reject({channel, source(true, "otherClock"), source(false, "clock"), sink}, "base clock");
  reject({channel, source(true, "clock"), source(false, "clock"),
    b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TriggerSink)),
      b.getNamedAttr("target", reference("credit")), b.getNamedAttr("clock", reference("clock"))})}, "nodes");
  auto body = dump(top);
  set({keep, sink});
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 0 &&
    dump(top) == body && circuit->getAttr("rawAnnotations") == b.getArrayAttr({keep}), "source-less sink changed hardware");
  set({keep, source(true, "clock"), source(false, "clock")});
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 2 &&
    dump(top) == body, "sink-less sources changed hardware");
  set({keep, channel, source(true, "clock"), source(false, "clock"), sink, sink});
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)), error);
  require(consumed == 2 && succeeded(verify(*root)), "invalid trigger IR");
  require(circuit->getAttr("rawAnnotations") == b.getArrayAttr({keep, channel}), "annotation cleanup/order");
  std::map<std::string, Value> values;
  unsigned registers = 0;
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) values[name.getValue().str()] = op->getResult(0);
    if (auto reg = dyn_cast<RegOp>(op)) {
      ++registers; require(reg.getClockVal() == top.getBodyBlock()->getArgument(0), "wrong clock");
    }
  });
  require(registers == 9, "Scala one-domain/sink architecture needs nine registers");
  auto width = [&](StringRef name) {
    return *cast<UIntType>(values.at(name.str()).getType()).getWidth();
  };
  auto driver = [&](Value dest) {
    Value src;
    for (auto connect : top.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == dest) { require(!src, "duplicate driver"); src = connect.getSrc(); }
    require(bool(src), "missing register driver"); return src;
  };
  for (StringRef stem : {"clock_credits", "clock_debits"}) {
    std::string name = stem.str(), next = name + "_next";
    require(width(name) == 16 && width(next) == 17 && width(next + "_diff") == 17, "local oracle widths");
    auto trunc = driver(values.at(name)).getDefiningOp<BitsPrimOp>();
    require(trunc && trunc.getInput() == values.at(next) && trunc.getHi() == 15 && trunc.getLo() == 0,
            "local update must truncate next, not the diff");
    require(driver(values.at(next + "_count_sync_s1")) == trunc.getResult() &&
      driver(values.at(next + "_count_sync_s2")) == values.at(next + "_count_sync_s1"), "sync must sample local NEXT");
    auto sub = values.at(next + "_diff").getDefiningOp<NodeOp>().getInput().getDefiningOp<SubPrimOp>();
    require(sub && sub.getLhs() == values.at(next + "_count_sync_s1") &&
      sub.getRhs() == values.at(next + "_count_sync_s2"), "wrong count difference");
  }
  for (StringRef name : {"totalCredits", "totalDebits"}) {
    require(width(name) == 32 && width(name.str() + "_next") == 33, "global oracle widths");
    auto trunc = driver(values.at(name.str())).getDefiningOp<BitsPrimOp>();
    require(trunc && trunc.getHi() == 31 && trunc.getInput() == values.at(name.str() + "_next"), "global update truncation");
  }
  auto compare = values.at("trigger_source").getDefiningOp<NodeOp>().getInput().getDefiningOp<NEQPrimOp>();
  require(compare && compare.getLhs() == values.at("totalCredits_next") &&
    compare.getRhs() == values.at("totalDebits_next"), "trigger must compare full 33-bit NEXT values");
  require(sinkNode.getInput() == values.at("trigger_sync") &&
    driver(values.at("trigger_sync")) == values.at("trigger_source"), "sink synchronizer wiring");
  require((values.count("credit_masked") == 1) == mask && (values.count("debit_masked") == 1) == mask, "reset mask optionality");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write candidate"); root->print(out); out << '\n';
  }
  auto completed = dump(root.get());
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 0 &&
    completed == dump(root.get()), "non-idempotent cleanup");
}
}
int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context, true, true, argc > 1 ? argv[1] : "");
    run(context, false, false, "");
    llvm::outs() << "TriggerWiring local accounting and atomic preflight PASS\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
