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
#include <functional>
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
  reject({channel, source(true, "clock"), source(true, "clock"), source(false, "clock"), sink}, "distinct source");
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
void multiple(MLIRContext &context, unsigned credits, unsigned debits, StringRef output) {
  std::string text = "module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { "
    "firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>";
  for (bool credit : {true, false})
    for (unsigned i = 0; i < (credit ? credits : debits); ++i)
      text += ", in %" + std::string(credit ? "credit" : "debit") + std::to_string(i) + ": !firrtl.uint<1>";
  text += ", out %enabled: !firrtl.uint<1>) {} } }";
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "multiple-source parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context); b.setInsertionPointToEnd(top.getBodyBlock());
  auto zero = b.create<ConstantOp>(top.getLoc(), UIntType::get(&context, 1), llvm::APInt(1, 0));
  auto node = b.create<NodeOp>(top.getLoc(), zero.getResult(), b.getStringAttr("trigger"));
  b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArguments().back(), node.getResult());
  auto ref = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  SmallVector<Attribute> annotations;
  annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("clock")}))}));
  for (bool credit : {true, false})
    for (unsigned i = 0; i < (credit ? credits : debits); ++i) {
      NamedAttrList a;
      a.set("class", b.getStringAttr(A::TriggerSource));
      a.set("target", ref(std::string(credit ? "credit" : "debit") + std::to_string(i)));
      a.set("clock", ref("clock")); a.set("sourceType", b.getBoolAttr(credit));
      if (i % 2 == 0) a.set("reset", ref("reset"));
      annotations.push_back(a.getDictionary(&context));
    }
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TriggerSink)),
    b.getNamedAttr("target", ref("trigger")), b.getNamedAttr("clock", ref("clock"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  unsigned consumed; std::string error;
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)), error);
  require(consumed == credits + debits && succeeded(verify(*root)), "multiple-source verification");
  std::map<std::string, Value> values;
  unsigned regs = 0, adds = 0;
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) values[name.getValue().str()] = op->getResult(0);
    regs += isa<RegOp>(op); adds += isa<AddPrimOp>(op);
  });
  unsigned prefixAdds = 0;
  for (unsigned n : {credits, debits})
    for (unsigned offset = 1; offset < n; offset *= 2) prefixAdds += n - offset;
  require(regs == 9 && adds == prefixAdds + 4, "Scala prefix topology/register count");
  // Evaluate the generated CIRCT reduction for every input/reset pattern in
  // small cases and 4096 patterns in larger cases. All-one patterns detect
  // accidentally truncated intermediate sums.
  uint64_t pattern = 0;
  std::function<uint64_t(Value)> eval = [&](Value v) -> uint64_t {
    uint64_t result;
    if (auto arg = dyn_cast<BlockArgument>(v)) result = (pattern >> (arg.getArgNumber() - 1)) & 1;
    else if (auto n = v.getDefiningOp<NodeOp>()) result = eval(n.getInput());
    else if (auto n = v.getDefiningOp<NotPrimOp>()) result = ~eval(n.getInput());
    else if (auto n = v.getDefiningOp<AndPrimOp>()) result = eval(n.getLhs()) & eval(n.getRhs());
    else if (auto n = v.getDefiningOp<AddPrimOp>()) result = eval(n.getLhs()) + eval(n.getRhs());
    else throw std::runtime_error("unexpected reduction operation");
    return result & ((uint64_t(1) << *cast<UIntType>(v.getType()).getWidth()) - 1);
  };
  unsigned patterns = 1u << std::min(credits + debits + 1, 12u);
  for (unsigned p = 0; p <= patterns; ++p) {
    pattern = p == patterns ? (uint64_t(1) << (credits + debits + 1)) - 2 : p;
    unsigned index = 1;
    for (bool credit : {true, false}) {
      unsigned sum = 0, n = credit ? credits : debits, width = 1;
      for (unsigned i = 0; i < n; ++i, ++index)
        sum += ((pattern >> index) & 1) && (i % 2 || !(pattern & 1));
      for (unsigned capacity = 1; capacity < n; capacity *= 2) ++width;
      auto add = values.at(credit ? "clock_credits_next" : "clock_debits_next")
        .getDefiningOp<NodeOp>().getInput().getDefiningOp<AddPrimOp>();
      require(*cast<UIntType>(add.getRhs().getType()).getWidth() == width && eval(add.getRhs()) == sum,
              "multiple-source sum/reset/width differs from Scala oracle");
    }
  }
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write multiple-source candidate"); root->print(out); out << '\n';
  }
}
void aliases(MLIRContext &context, unsigned mode, StringRef output,
             unsigned hierarchy = 0) {
  // Augmented Scala TriggerWiring oracle: credit reset mask, unmasked debit,
  // distinct local aliases of one clock. SFC emits root-clock local counters,
  // baseAlias global/sampling registers and a sinkAlias synchronizer. SFC also
  // rejects a sink clock alias declared after the sink node (mode 6).
  std::string body, children;
  if (hierarchy) {
    body = R"mlir(
      %left:2 = firrtl.instance left @Forward(in clock_in: !firrtl.clock, out clock_out: !firrtl.clock)
      %right:2 = firrtl.instance right @Forward(in clock_in: !firrtl.clock, out clock_out: !firrtl.clock)
      firrtl.strictconnect %left#0, %clock : !firrtl.clock
    )mlir";
    if (hierarchy != 4)
      body += "firrtl.strictconnect %right#0, %" + std::string(hierarchy == 3 ? "otherClock" : "clock") + " : !firrtl.clock\n";
    children = "firrtl.module @Forward(in %clock_in: !firrtl.clock, out %clock_out: !firrtl.clock) {\n";
    if (hierarchy == 2) {
      children += R"mlir(
        %relay:2 = firrtl.instance relay @Relay(in clock_in: !firrtl.clock, out clock_out: !firrtl.clock)
        firrtl.strictconnect %relay#0, %clock_in : !firrtl.clock
        firrtl.strictconnect %clock_out, %relay#1 : !firrtl.clock
      } firrtl.module @Relay(in %clock_in: !firrtl.clock, out %clock_out: !firrtl.clock) {
        %alias = firrtl.node %clock_in : !firrtl.clock
        firrtl.strictconnect %clock_out, %alias : !firrtl.clock
      )mlir";
    } else if (hierarchy == 6) {
      children += "firrtl.connect %clock_out, %clock_out : !firrtl.clock, !firrtl.clock\n";
    } else {
      if (hierarchy == 7)
        children += "%one = firrtl.constant 1 : !firrtl.uint<1>\nfirrtl.when %one : !firrtl.uint<1> {\n";
      children += "firrtl.strictconnect %clock_out, %clock_in : !firrtl.clock\n";
      if (hierarchy == 7) children += "}\n";
      if (hierarchy == 5)
        children += "firrtl.strictconnect %clock_out, %clock_in : !firrtl.clock\n";
    }
    children += "}\n";
    if (hierarchy == 8)
      children = "firrtl.extmodule @Forward(in clock_in: !firrtl.clock, out clock_out: !firrtl.clock)\n";
  }
  body += R"mlir(
    %baseAlias = firrtl.wire : !firrtl.clock
    %creditAlias = firrtl.node %baseAlias : !firrtl.clock
    %debitAlias = firrtl.wire : !firrtl.clock
  )mlir";
  if (mode != 6) body += "%sinkAlias = firrtl.node %debitAlias : !firrtl.clock\n";
  body += R"mlir(
    %zero = firrtl.constant 0 : !firrtl.uint<1>
    %trigger = firrtl.node %zero : !firrtl.uint<1>
    firrtl.strictconnect %enabled, %trigger : !firrtl.uint<1>
  )mlir";
  if (mode == 6) body += "%sinkAlias = firrtl.node %debitAlias : !firrtl.clock\n";
  body += "firrtl.strictconnect %baseAlias, %" + std::string(hierarchy ? "left#1" : "clock") + " : !firrtl.clock\n";
  if (mode == 2) body += "firrtl.strictconnect %debitAlias, %otherClock : !firrtl.clock\n";
  if (mode != 5) {
    if (mode == 7) body += "firrtl.when %reset : !firrtl.uint<1> {\n";
    body += "firrtl.connect %debitAlias, %" + std::string(hierarchy ? "right#1" : mode == 3 ? "sinkAlias" : mode == 4 ? "otherClock" : "creditAlias") + " : !firrtl.clock, !firrtl.clock\n";
    if (mode == 7) body += "}\n";
  }
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { "
    "firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, "
    "in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, "
    "in %otherClock: !firrtl.clock, out %enabled: !firrtl.uint<1>) {" + body + "}" + children + "} }", &context);
  require(bool(root), "clock alias parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto ref = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  SmallVector<Attribute> annotations;
  annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("baseAlias")}))}));
  for (bool credit : {true, false}) {
    NamedAttrList a;
    a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref(credit ? "credit" : "debit"));
    a.set("clock", ref(credit ? "creditAlias" : "debitAlias"));
    a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref("reset"));
    annotations.push_back(a.getDictionary(&context));
  }
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
    b.getNamedAttr("target", ref("trigger")), b.getNamedAttr("clock", ref("sinkAlias"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(root.get());
  unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode || hierarchy > 2) {
    require(failed(result) && consumed == 0 && dump(root.get()) == before,
            "clock alias rejection must be atomic: " + std::to_string(mode) + ":" + std::to_string(hierarchy));
    require(StringRef(error).contains(mode == 6 ? "dominate" : "base clock"),
            "clock alias diagnostic: " + error);
    return;
  }
  require(succeeded(result), error);
  require(consumed == 2 && succeeded(verify(*root)), "clock alias candidate invalid");
  std::map<std::string, Value> values;
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) values[name.getValue().str()] = op->getResult(0);
  });
  unsigned registers = 0;
  top.walk([&](RegOp reg) {
    ++registers;
    auto name = reg.getName();
    Value expected = name == "trigger_sync" ? values.at("sinkAlias") :
      (name == "clock_credits" || name == "clock_debits") ? top.getBodyBlock()->getArgument(0) : values.at("baseAlias");
    require(reg.getClockVal() == expected, "Scala alias register clock identity: " + name.str());
  });
  require(registers == 9, "clock aliases must merge one accounting domain");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "clock alias annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write alias candidate"); root->print(out); out << '\n';
  }
}

}
int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context, true, true, argc > 1 ? argv[1] : "");
    run(context, false, false, "");
    multiple(context, 3, 5, argc > 2 ? argv[2] : "");
    multiple(context, 2, 3, "");
    multiple(context, 4, 5, "");
    multiple(context, 5, 7, "");
    multiple(context, 17, 2, "");
    aliases(context, 0, argc > 3 ? argv[3] : "");
    for (unsigned mode : {2, 3, 4, 5, 6, 7}) aliases(context, mode, "");
    aliases(context, 0, argc > 4 ? argv[4] : "", 1);
    aliases(context, 0, argc > 5 ? argv[5] : "", 2);
    for (unsigned hierarchy : {3, 4, 5, 6, 7, 8}) aliases(context, 0, "", hierarchy);
    llvm::outs() << "TriggerWiring local accounting and atomic preflight PASS\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
