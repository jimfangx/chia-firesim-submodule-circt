// See LICENSE for license details.
#include "goldengate/TriggerWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/ImplicitLocOpBuilder.h"
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
    auto result = goldengate::wireTriggers(circuit, consumed, error);
    require(failed(result) && consumed == 0 &&
      dump(root.get()) == before && StringRef(error).contains(reason),
      "non-atomic rejection (expected " + reason.str() + ", consumed " +
        std::to_string(consumed) + "): " + error);
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
  std::string leftClock = "left#1", rightClock = "right#1";
  if (hierarchy && hierarchy < 9) {
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
  if (hierarchy >= 9) {
    // The immutable Rocket handoff uses trace.traces[0].clock. Aggregate
    // connects and repeated projections must retain each leaf's identity.
    const std::string traceType = "!firrtl.bundle<traces: vector<bundle<clock: clock>, 2>>";
    body = "%left:3 = firrtl.instance left @Forward(in clock_in: !firrtl.clock, in other: !firrtl.clock, out trace: " + traceType + ")\n";
    body += "%right:3 = firrtl.instance right @Forward(in clock_in: !firrtl.clock, in other: !firrtl.clock, out trace: " + traceType + ")\n";
    for (auto name : {"left", "right"}) {
      body += "firrtl.strictconnect %" + std::string(name) + "#0, %clock : !firrtl.clock\n";
      body += "firrtl.strictconnect %" + std::string(name) + "#1, %otherClock : !firrtl.clock\n";
    }
    // Parent and child whole-bundle forwarding exercise recursive indexing.
    body += "%forwarded = firrtl.wire : " + traceType + "\n";
    if (hierarchy == 14) body += "firrtl.when %reset : !firrtl.uint<1> {\n";
    body += "firrtl.strictconnect %forwarded, %left#2 : " + traceType + "\n";
    if (hierarchy == 14) body += "}\n";
    auto project = [&](std::string name, std::string value, unsigned index) {
      body += "%" + name + "_traces = firrtl.subfield %" + value + "[traces] : " + traceType + "\n";
      body += "%" + name + "_entry = firrtl.subindex %" + name + "_traces[" + std::to_string(index) + "] : !firrtl.vector<bundle<clock: clock>, 2>\n";
      body += "%" + name + " = firrtl.subfield %" + name + "_entry[clock] : !firrtl.bundle<clock: clock>\n";
    };
    body += "%forwardedNode = firrtl.node %forwarded : " + traceType + "\n";
    project("leftClock", "forwardedNode", 0);
    project("rightClock", "right#2", hierarchy == 11 ? 1 : 0);
    leftClock = "leftClock"; rightClock = "rightClock";
    children = "firrtl.module @Forward(in %clock_in: !firrtl.clock, in %other: !firrtl.clock, out %trace: " + traceType + ") {\n";
    children += "%wire = firrtl.wire : " + traceType + "\nfirrtl.strictconnect %trace, %wire : " + traceType + "\n";
    children += "%traces = firrtl.subfield %wire[traces] : " + traceType + "\n";
    children += "%entry0 = firrtl.subindex %traces[0] : !firrtl.vector<bundle<clock: clock>, 2>\n";
    children += "%entry1 = firrtl.subindex %traces[1] : !firrtl.vector<bundle<clock: clock>, 2>\n";
    children += "%field0 = firrtl.subfield %entry0[clock] : !firrtl.bundle<clock: clock>\n";
    children += "%field1 = firrtl.subfield %entry1[clock] : !firrtl.bundle<clock: clock>\n";
    if (hierarchy != 12) children += "firrtl.strictconnect %field0, %clock_in : !firrtl.clock\n";
    children += "firrtl.strictconnect %field1, %other : !firrtl.clock\n";
    // Same leaf, different SSA projection: duplicate driver is still ambiguous.
    if (hierarchy == 13) children += "%duplicate = firrtl.subfield %entry0[clock] : !firrtl.bundle<clock: clock>\nfirrtl.strictconnect %duplicate, %clock_in : !firrtl.clock\n";
    children += "}\n";
    if (hierarchy == 10) {
      // The flipped input leaf of an output bundle is an input to the child.
      const std::string flipped = "!firrtl.bundle<in flip: clock, out: clock>";
      body = "%left = firrtl.instance left @Forward(out io: " + flipped + ")\n%right = firrtl.instance right @Forward(out io: " + flipped + ")\n";
      for (auto name : {"left", "right"}) {
        body += "%" + std::string(name) + "Wire = firrtl.wire : " + flipped + "\n";
        body += "firrtl.connect %" + std::string(name) + "Wire, %" + name + " : " + flipped + ", " + flipped + "\n";
        body += "%" + std::string(name) + "Input = firrtl.subfield %" + name + "Wire[in] : " + flipped + "\n";
        body += "firrtl.strictconnect %" + std::string(name) + "Input, %clock : !firrtl.clock\n";
        body += "%" + std::string(name) + "Clock = firrtl.subfield %" + name + "Wire[out] : " + flipped + "\n";
      }
      children = "firrtl.module @Forward(out %io: " + flipped + ") {\n%input = firrtl.subfield %io[in] : " + flipped + "\n%output = firrtl.subfield %io[out] : " + flipped + "\nfirrtl.strictconnect %output, %input : !firrtl.clock\n}\n";
    }
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
  body += "firrtl.strictconnect %baseAlias, %" + std::string(hierarchy ? leftClock : "clock") + " : !firrtl.clock\n";
  if (mode == 2) body += "firrtl.strictconnect %debitAlias, %otherClock : !firrtl.clock\n";
  if (mode != 5) {
    if (mode == 7) body += "firrtl.when %reset : !firrtl.uint<1> {\n";
    body += "firrtl.connect %debitAlias, %" + std::string(hierarchy ? rightClock : mode == 3 ? "sinkAlias" : mode == 4 ? "otherClock" : "creditAlias") + " : !firrtl.clock, !firrtl.clock\n";
    if (mode == 7) body += "}\n";
  }
  bool aggregateRoot = hierarchy >= 15;
  std::string clockPort = "in %clock: !firrtl.clock";
  if (aggregateRoot) {
    // Actual Scala normalization followed by TriggerWiring emits local counters
    // named clocks_traces_0_clock_{credits,debits}. Native CIRCT retains this
    // nested aggregate input and must select its leaf, not the entire argument.
    clockPort = "in %clocks: !firrtl.bundle<traces: vector<bundle<clock: clock>, 2>>";
    if (hierarchy == 16) {
      auto connect = body.find("firrtl.strictconnect %right#0, %clock :");
      require(connect != std::string::npos, "missing aggregate-root child input");
      body.replace(connect, std::string("firrtl.strictconnect %right#0, %clock :").size(),
                   "firrtl.strictconnect %right#0, %otherRoot :");
    }
    body = R"mlir(
      %rootTraces = firrtl.subfield %clocks[traces] : !firrtl.bundle<traces: vector<bundle<clock: clock>, 2>>
      %rootEntry = firrtl.subindex %rootTraces[0] : !firrtl.vector<bundle<clock: clock>, 2>
      %clock = firrtl.subfield %rootEntry[clock] : !firrtl.bundle<clock: clock>
      %otherEntry = firrtl.subindex %rootTraces[1] : !firrtl.vector<bundle<clock: clock>, 2>
      %otherRoot = firrtl.subfield %otherEntry[clock] : !firrtl.bundle<clock: clock>
    )mlir" + body;
    if (hierarchy == 17) {
      auto index = body.find("firrtl.subindex %rootTraces[0]");
      require(index != std::string::npos, "missing aggregate-root projection");
      body.replace(index, std::string("firrtl.subindex %rootTraces[0]").size(),
                   "firrtl.subindex %rootTraces[1]");
    }
  }
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { "
    "firrtl.module @Top(" + clockPort + ", in %reset: !firrtl.uint<1>, "
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
  if (mode || (hierarchy > 2 && hierarchy != 9 && hierarchy != 10 && hierarchy != 15 && hierarchy != 17)) {
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
  std::string clockName = aggregateRoot ?
    (hierarchy == 17 ? "clocks_traces_1_clock" : "clocks_traces_0_clock") : "clock";
  circt::FieldRef expectedRoot(top.getBodyBlock()->getArgument(0),
                             aggregateRoot ? (hierarchy == 17 ? 5 : 3) : 0);
  top.walk([&](RegOp reg) {
    ++registers;
    auto name = reg.getName();
    if (name == clockName + "_credits" || name == clockName + "_debits")
      require(getFieldRefFromValue(reg.getClockVal()) == expectedRoot,
              "Scala aggregate root clock identity: " + name.str());
    else
      require(reg.getClockVal() == values.at(name == "trigger_sync" ? "sinkAlias" : "baseAlias"),
              "Scala alias register clock identity: " + name.str());
  });
  require(values.count(clockName + "_credits") && values.count(clockName + "_debits"),
          "Scala flattened root counter names");
  require(registers == 9, "clock aliases must merge one accounting domain");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "clock alias annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write alias candidate"); root->print(out); out << '\n';
  }
}

// Raw aggregate clock targets correspond to Scala's renamed LowerTypes leaves.
// Keep event/reset/sink-node targets ground and test every failure before IR
// projection materialization, including a late aggregate sink declaration.
void clockTargets(MLIRContext &context, unsigned mode, StringRef output) {
  const std::string type = "!firrtl.bundle<traces: vector<bundle<clock: clock>, 2>, flag: uint<1>>";
  std::string body;
  if (mode == 2) {
    body = "%base = firrtl.wire : " + type + "\nfirrtl.strictconnect %base, %clocks : " + type + "\n";
  } else body = "%base = firrtl.node %clocks : " + type + "\n";
  if (mode != 7) body += "%sink = firrtl.node %base : " + type + "\n";
  body += "%one = firrtl.constant 1 : !firrtl.uint<1>\n%trigger = firrtl.node %one : !firrtl.uint<1>\nfirrtl.strictconnect %enabled, %trigger : !firrtl.uint<1>\n";
  if (mode == 7) body += "%sink = firrtl.node %base : " + type + "\n";
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { firrtl.module @Top(in %clocks: " + type +
    ", in %reset: !firrtl.uint<1>, in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, out %enabled: !firrtl.uint<1>) {" + body + "} } }", &context);
  require(bool(root), "aggregate clock target parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto ref = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  std::string base = mode == 0 ? "clocks.traces[0].clock" : "base.traces[0].clock";
  if (mode == 4) base = "base.traces[2].clock";
  if (mode == 5) base = "clocks.traces";
  if (mode == 6) base = "clocks.flag";
  if (mode == 8) base = "base.traces[0].missing";
  SmallVector<Attribute> annotations;
  annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref(base)}))}));
  for (bool credit : {true, false}) {
    NamedAttrList a;
    a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref(credit ? "credit" : "debit"));
    a.set("clock", ref(credit ? "clocks.traces[0].clock" :
                          mode == 3 ? "base.traces[1].clock" :
                          mode == 9 ? "base.traces[-1].clock" : base));
    a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref("reset"));
    annotations.push_back(a.getDictionary(&context));
  }
  std::string sink = mode == 0 ? "clocks.traces[0].clock" :
                     mode == 10 ? "sink.traces[0].clock.missing" : "sink.traces[0].clock";
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
    b.getNamedAttr("target", ref("trigger")), b.getNamedAttr("clock", ref(sink))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(root.get());
  unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode >= 3) {
    require(failed(result) && consumed == 0 && dump(root.get()) == before,
            "aggregate clock target rejection must be atomic: " + std::to_string(mode));
    require(!error.empty(), "missing aggregate clock diagnostic");
    return;
  }
  require(succeeded(result), error);
  require(consumed == 2 && succeeded(verify(*root)), "aggregate clock target candidate invalid");
  std::map<std::string, Value> values;
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) values[name.getValue().str()] = op->getResult(0);
  });
  unsigned registers = 0;
  top.walk([&](RegOp reg) {
    ++registers;
    bool local = reg.getName() == "clocks_traces_0_clock_credits" || reg.getName() == "clocks_traces_0_clock_debits";
    Value expected = local || mode == 0 ? top.getBodyBlock()->getArgument(0) :
                     values.at(reg.getName() == "trigger_sync" ? "sink" : "base");
    require(getFieldRefFromValue(reg.getClockVal()) == circt::FieldRef(expected, 3),
            "aggregate annotation clock operand identity: " + reg.getName().str());
  });
  require(registers == 9 && values.count("clocks_traces_0_clock_credits") &&
          values.count("clocks_traces_0_clock_debits"), "aggregate clock target accounting domain");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "aggregate clock annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write aggregate clock target candidate"); root->print(out); out << '\n';
  }
}
// Aggregate event/reset references are renamed to these same UInt leaves by
// Scala LowerTypes. Failed leaf selection must not leave projections behind.
void eventTargets(MLIRContext &context, unsigned mode, StringRef output) {
  const std::string type = "!firrtl.bundle<events: vector<bundle<credit: uint<1>, debit: uint<1>, reset: uint<1>>, 2>, wide: uint<2>, clock: clock>";
  std::string body = mode == 2 ?
    "%data = firrtl.wire : " + type + "\nfirrtl.strictconnect %data, %payload : " + type + "\n" :
    "%data = firrtl.node %payload : " + type + "\n";
  body += "%one = firrtl.constant 1 : !firrtl.uint<1>\n%trigger = firrtl.node %one : !firrtl.uint<1>\nfirrtl.strictconnect %enabled, %trigger : !firrtl.uint<1>\n";
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { firrtl.module @Top(in %clock: !firrtl.clock, in %payload: " + type + ", out %enabled: !firrtl.uint<1>) {" + body + "} } }", &context);
  require(bool(root), "aggregate event target parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto ref = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  std::string stem = mode == 0 || mode >= 3 ? "payload" : "data";
  std::string credit = stem + ".events[0].credit", debit = stem + ".events[1].debit";
  std::string reset = stem + ".events[0].reset";
  if (mode == 3) credit = "payload.events";
  if (mode == 4) credit = "payload.events[2].credit";
  if (mode == 5) credit = "payload.events[0].missing";
  if (mode == 6) credit = "payload.wide";
  if (mode == 7) credit = "payload.clock";
  if (mode == 8) reset = "payload.wide";
  if (mode == 9) reset = "payload.events[2].reset";
  SmallVector<Attribute> annotations;
  annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("clock")}))}));
  auto addSource = [&](StringRef target, bool isCredit) {
    NamedAttrList a;
    a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref(target)); a.set("clock", ref("clock"));
    a.set("sourceType", b.getBoolAttr(isCredit));
    if (isCredit) a.set("reset", ref(reset));
    annotations.push_back(a.getDictionary(&context));
  };
  addSource(credit, true); addSource(debit, false);
  if (mode == 10) addSource("payload.events.0.credit", true);
  // Two leaves of the same aggregate SSA value are distinct credit sources.
  if (mode == 11) addSource("payload.events[1].credit", true);
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
    b.getNamedAttr("target", ref("trigger")), b.getNamedAttr("clock", ref("clock"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(root.get());
  unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode >= 3 && mode != 11) {
    require(failed(result) && consumed == 0 && dump(root.get()) == before,
            "aggregate event/reset rejection must be atomic: " + std::to_string(mode));
    require(!error.empty(), "missing aggregate event/reset diagnostic"); return;
  }
  require(succeeded(result), error);
  require(consumed == (mode == 11 ? 3 : 2) && succeeded(verify(*root)), "aggregate event candidate invalid");
  unsigned registers = 0, masked = 0;
  Value eventRoot = mode == 1 || mode == 2 ? Value() : top.getBodyBlock()->getArgument(1);
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"); name && name == "data" && (mode == 1 || mode == 2)) eventRoot = op->getResult(0);
  });
  top.walk([&](RegOp reg) { ++registers; });
  top.walk([&](NodeOp node) {
    if (node.getName() != stem + "_events_0_credit_masked") return;
    ++masked;
    auto gate = node.getInput().getDefiningOp<AndPrimOp>();
    require(bool(gate), "aggregate reset mask AND");
    auto negate = gate.getLhs().getDefiningOp<NotPrimOp>();
    require(bool(negate), "aggregate reset mask NOT");
    require(getFieldRefFromValue(gate.getRhs()) == circt::FieldRef(eventRoot, 3) &&
            getFieldRefFromValue(negate.getInput()) == circt::FieldRef(eventRoot, 5),
            "selected aggregate event/reset mask identities mode=" + std::to_string(mode) + " event=" + std::to_string(getFieldRefFromValue(gate.getRhs()).getFieldID()) + " reset=" + std::to_string(getFieldRefFromValue(negate.getInput()).getFieldID()));
  });
  require(registers == 9 && masked == 1, "aggregate event accounting and Scala flattened mask name");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "aggregate event annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write aggregate event candidate"); root->print(out); out << '\n';
  }
}

// Source events belong to the child definition; reset masking must happen there
// before the exported event reaches the top accounting domain.
void childSources(MLIRContext &context, unsigned mode, StringRef output, unsigned hierarchy = 0) {
  std::string payload = "!firrtl.bundle<events: vector<bundle<credit: uint<1>, debit: uint<1>, reset: uint<1>>, 2>>";
  std::string childPorts = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>";
  if (mode == 1) childPorts += ", in %payload: " + payload;
  std::string instancePorts = "in clock: !firrtl.clock, in credit: !firrtl.uint<1>, in debit: !firrtl.uint<1>, in reset: !firrtl.uint<1>";
  if (mode == 1) instancePorts += ", in payload: " + payload;
  childPorts += ", out %echo: !firrtl.uint<1>";
  instancePorts += ", out echo: !firrtl.uint<1>";
  auto inst = [&](StringRef name, StringRef module) {
    return "%" + name.str() + ":" + (mode == 1 ? "6" : "5") +
      " = firrtl.instance " + name.str() + " @" + module.str() + "(" + instancePorts + ")";
  };
  std::string relays;
  std::string lastModule = "Child";
  for (unsigned level = 0; level < hierarchy; ++level) {
    std::string module = level == 0 ? "Relay" : "Relay" + std::to_string(level);
    relays += " firrtl.module @" + module + "(" + childPorts + ") { " +
      inst(level == 0 ? "child" : "middle", lastModule) + " }";
    lastModule = module;
  }
  std::string text = "module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { firrtl.module @Child(" + childPorts + ") {}" + relays + " firrtl.module @Top(in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, in %otherClock: !firrtl.clock, out %enabled: !firrtl.uint<1>, out %echo: !firrtl.uint<1>) { " + inst(hierarchy ? "relay" : "child", lastModule) + " } } }";
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "parse child source fixture");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>(); auto it = modules.begin();
  auto child = *it++; SmallVector<FModuleOp> relayModules;
  for (unsigned level = 0; level < hierarchy; ++level) relayModules.push_back(*it++);
  auto top = *it;
  auto instance = *top.getBodyBlock()->getOps<InstanceOp>().begin();
  OpBuilder b(&context); auto loc = top.getLoc();
  instance->setAttr("example.metadata", b.getStringAttr("preserve"));
  for (auto relay : relayModules) {
    auto nested = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    nested->setAttr("example.metadata", b.getStringAttr("preserve"));
    b.setInsertionPointToEnd(relay.getBodyBlock());
    for (unsigned i = 0; i < relay.getNumPorts() - 1; ++i)
      b.create<StrictConnectOp>(loc, nested.getResult(i), relay.getBodyBlock()->getArgument(i));
    b.create<StrictConnectOp>(loc, relay.getBodyBlock()->getArgument(relay.getNumPorts() - 1),
                              nested.getResult(relay.getNumPorts() - 1));
    if (mode == 8) {
      b.create<NodeOp>(loc, relay.getBodyBlock()->getArgument(1), b.getStringAttr("relayCredit"));
      b.create<NodeOp>(loc, relay.getBodyBlock()->getArgument(2), b.getStringAttr("relayDebit"));
    }
  }
  b.setInsertionPointToEnd(child.getBodyBlock());
  if (mode == 6) b.create<NodeOp>(loc, child.getBodyBlock()->getArgument(0), b.getStringAttr("clockAlias"));
  Value credit = child.getBodyBlock()->getArgument(1), debit = child.getBodyBlock()->getArgument(2);
  if (mode == 1) {
    ImplicitLocOpBuilder fields(loc, b);
    credit = getValueByFieldID(fields, child.getBodyBlock()->getArgument(4), 3);
    debit = getValueByFieldID(fields, child.getBodyBlock()->getArgument(4), 8);
  }
  auto creditNode = b.create<NodeOp>(loc, credit, b.getStringAttr("creditEvent"));
  b.create<StrictConnectOp>(loc, child.getBodyBlock()->getArgument(child.getNumPorts() - 1), creditNode.getResult());
  b.create<NodeOp>(loc, debit, b.getStringAttr("debitEvent"));
  b.setInsertionPointToEnd(top.getBodyBlock());
  for (unsigned i = 0; i < 4; ++i)
    b.create<StrictConnectOp>(loc, instance.getResult(i), top.getBodyBlock()->getArgument(mode == 3 && i == 0 ? 4 : i));
  if (mode == 1) {
    ImplicitLocOpBuilder fields(loc, b);
    auto zero = b.create<ConstantOp>(loc, UIntType::get(&context, 1), llvm::APInt(1, 0));
    for (auto [id, src] : SmallVector<std::pair<unsigned, Value>>{{3, top.getBodyBlock()->getArgument(1)}, {4, zero}, {5, top.getBodyBlock()->getArgument(3)}, {7, zero}, {8, top.getBodyBlock()->getArgument(2)}, {9, zero}})
      b.create<StrictConnectOp>(loc, getValueByFieldID(fields, instance.getResult(4), id), src);
  }
  b.create<StrictConnectOp>(loc, top.getBodyBlock()->getArgument(6), instance.getResult(child.getNumPorts() - 1));
  if (mode == 2) {
    auto other = cast<InstanceOp>(b.clone(*instance.getOperation()));
    other.setNameAttr(b.getStringAttr("other"));
  }
  if (mode == 7) {
    // The source definition itself is unique, but an ancestor is repeated.
    auto relay = relayModules.front();
    auto nested = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    auto other = cast<InstanceOp>(b.clone(*nested.getOperation()));
    other.setModuleNameAttr(FlatSymbolRefAttr::get(&context, relay.getName()));
    other.setNameAttr(b.getStringAttr("otherRelay"));
  }
  auto one = b.create<ConstantOp>(loc, UIntType::get(&context, 1), llvm::APInt(1, 1));
  auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
  b.create<StrictConnectOp>(loc, top.getBodyBlock()->getArgument(5), trigger.getResult());
  auto ref = [&](StringRef module, StringRef name) { return b.getStringAttr(("~Top|" + module + ">" + name).str()); };
  SmallVector<Attribute> annos;
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)), b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})), b.getNamedAttr("sinks", b.getArrayAttr({ref("Top", "clock")}))}));
  if (mode == 8) for (bool credit : {true, false}) {
    // Deliberately plan the ancestor before its descendant: emission must sort
    // the modules by depth and append all of the ancestor's exports together.
    NamedAttrList a; a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref("Relay", credit ? "relayCredit" : "relayDebit"));
    a.set("clock", ref("Relay", "clock")); a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref("Relay", "reset"));
    annos.push_back(a.getDictionary(&context));
  }
  for (bool credit : {true, false}) {
    NamedAttrList a; a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref("Child", credit ? "creditEvent" : "debitEvent"));
    a.set("clock", ref("Child", mode == 6 ? "clockAlias" : "clock")); a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref(mode == 4 ? "Top" : "Child", mode == 1 ? "payload.events[0].reset" : "reset"));
    annos.push_back(a.getDictionary(&context));
  }
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)), b.getNamedAttr("target", ref("Top", mode == 5 ? "credit" : "trigger")), b.getNamedAttr("clock", ref("Top", "clock"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  auto before = dump(root.get()); unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode >= 2 && mode != 6 && mode != 8) {
    require(failed(result) && consumed == 0 && !error.empty() && dump(root.get()) == before,
            "child source failure must preserve module IO, instances and annotations: " + std::to_string(mode)); return;
  }
  require(succeeded(result), error);
  require(consumed == (mode == 8 ? 4 : 2) && succeeded(verify(*root)), "invalid child trigger candidate");
  require(top.getNumPorts() == 7 && child.getNumPorts() == (mode == 1 ? 8 : 7), "child event exports changed original IO");
  auto replacement = *top.getBodyBlock()->getOps<InstanceOp>().begin();
  bool echoPreserved = false;
  for (auto connect : top.getBodyBlock()->getOps<StrictConnectOp>())
    if (connect.getDest() == top.getBodyBlock()->getArgument(6))
      echoPreserved = connect.getSrc() == replacement.getResult(child.getNumPorts() - 3);
  require(echoPreserved, "existing child output result use lost during replacement");
  require(replacement->getAttrOfType<StringAttr>("example.metadata") == "preserve", "parent instance metadata lost");
  std::string prefix = "child_";
  for (auto relay : relayModules) {
    require(relay.getNumPorts() == child.getNumPorts() + (mode == 8 ? 2 : 0), "relay lost appended event ports");
    require(relay.getPortName(relay.getNumPorts() - 2) == "simulationTrigger_" + prefix + "creditEvent_masked" &&
            relay.getPortName(relay.getNumPorts() - 1) == "simulationTrigger_" + prefix + "debitEvent",
            "Scala intermediate event export names");
    auto nested = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    require(nested->getAttrOfType<StringAttr>("example.metadata") == "preserve", "nested instance metadata lost");
    bool echoConnected = false;
    for (auto connect : relay.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == relay.getBodyBlock()->getArgument(child.getNumPorts() - 3))
        echoConnected = connect.getSrc() == nested.getResult(child.getNumPorts() - 3);
    require(echoConnected, "relay original output connection lost");
    unsigned relayRegisters = 0, relayMasks = 0;
    relay.walk([&](RegOp) { ++relayRegisters; });
    relay.walk([&](NodeOp node) { if (node.getName() == "creditEvent_masked") ++relayMasks; });
    require(relayRegisters == 0 && relayMasks == 0, "relay duplicated masking or accounting");
    prefix = "middle_" + prefix;
  }
  require(child.getPortName(child.getNumPorts() - 2) == "simulationTrigger_creditEvent_masked" &&
          child.getPortName(child.getNumPorts() - 1) == "simulationTrigger_debitEvent", "Scala child event export names");
  unsigned registers = 0, masks = 0, childRegisters = 0;
  top.walk([&](RegOp reg) { ++registers; require(reg.getClockVal() == top.getBodyBlock()->getArgument(0), "child trigger top accounting clock"); });
  child.walk([&](RegOp reg) { ++childRegisters; });
  child.walk([&](NodeOp node) { if (node.getName() == "creditEvent_masked") ++masks; });
  require(registers == 9 && masks == 1 && childRegisters == 0, "child masking/top accounting placement");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "child annotation cleanup");
  if (!output.empty()) { std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, "write child source candidate"); root->print(out); out << '\n'; }
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
    aliases(context, 0, argc > 6 ? argv[6] : "", 9);
    aliases(context, 0, argc > 7 ? argv[7] : "", 10);
    for (unsigned hierarchy : {11, 12, 13, 14}) aliases(context, 0, "", hierarchy);
    aliases(context, 0, argc > 8 ? argv[8] : "", 15);
    aliases(context, 0, "", 16);
    aliases(context, 2, "", 15);
    aliases(context, 0, argc > 9 ? argv[9] : "", 17);
    clockTargets(context, 0, argc > 10 ? argv[10] : "");
    clockTargets(context, 1, argc > 11 ? argv[11] : "");
    clockTargets(context, 2, argc > 12 ? argv[12] : "");
    for (unsigned mode : {3, 4, 5, 6, 7, 8, 9, 10}) clockTargets(context, mode, "");
    eventTargets(context, 0, argc > 13 ? argv[13] : "");
    eventTargets(context, 1, argc > 14 ? argv[14] : "");
    eventTargets(context, 2, argc > 15 ? argv[15] : "");
    for (unsigned mode = 3; mode <= 11; ++mode) eventTargets(context, mode, "");
    childSources(context, 0, argc > 16 ? argv[16] : "");
    childSources(context, 1, argc > 17 ? argv[17] : "");
    childSources(context, 6, "");
    for (unsigned mode : {2, 3, 4, 5}) childSources(context, mode, "");
    childSources(context, 0, argc > 18 ? argv[18] : "", 1);
    childSources(context, 1, argc > 19 ? argv[19] : "", 1);
    childSources(context, 0, "", 2);
    childSources(context, 8, argc > 20 ? argv[20] : "", 1);
    for (unsigned mode : {2, 3, 4, 5, 7}) childSources(context, mode, "", 1);
    llvm::outs() << "TriggerWiring local accounting and atomic preflight PASS\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
