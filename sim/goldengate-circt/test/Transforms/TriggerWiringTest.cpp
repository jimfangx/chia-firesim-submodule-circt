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
#include "mlir/AsmParser/AsmParser.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
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
void run(MLIRContext &context, bool internal, bool mask, StringRef output,
         bool sharedTarget = false) {
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
    attrs.set("target", reference(credit || sharedTarget ? "credit" : "debit"));
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
  if (!sharedTarget) {
    if (!mask) {
      reject({channel, source(true, "clock"), source(true, "otherClock"), source(false, "clock"), sink}, "source clock domain");
      reject({channel, source(true, "clock"), source(true, "missingClock"), source(false, "clock"), sink}, "local field");
    }
    reject({channel, source(true, "otherClock"), source(false, "clock"), sink}, "source clock domain");
  } else {
    reject({channel, source(false, "missingClock"), source(true, "clock"), sink}, "local field");
  }
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
  SmallVector<Attribute> finalAnnotations{keep, channel, source(true, "clock"), source(false, "clock"), sink, sink};
  if (sharedTarget) {
    // Raw credits follow debits; Scala regroups them before clock selection.
    finalAnnotations = {keep, channel, source(false, "clock"),
                        source(true, "otherClock"), sink};
  } else if (!mask) {
    finalAnnotations.insert(finalAnnotations.begin() + 2, source(true, "otherClock"));
    finalAnnotations.push_back(source(true, "clock"));
  }
  set(finalAnnotations);
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)), error);
  require(consumed == (mask || sharedTarget ? 2 : 4) && succeeded(verify(*root)), "invalid trigger IR");
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
    if (sharedTarget) {
      auto add = values.at(next).getDefiningOp<NodeOp>().getInput().getDefiningOp<AddPrimOp>();
      require(add && add.getRhs() == top.getBodyBlock()->getArgument(1),
              "shared local target must increment both accounting lists once");
    }
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
    require(StringRef(error).contains(mode == 6 ? "dominate" : "clock"),
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
  bool occupiedMask = mode >= 12;
  if (occupiedMask) mode -= 12;
  const std::string type = "!firrtl.bundle<events: vector<bundle<credit: uint<1>, debit: uint<1>, reset: uint<1>" +
      std::string(occupiedMask ? ", credit_masked: uint<1>" : "") + ">, 2>, wide: uint<2>, clock: clock>";
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
  if (mode >= 3 && mode != 10 && mode != 11) {
    require(failed(result) && consumed == 0 && dump(root.get()) == before,
            "aggregate event/reset rejection must be atomic: " + std::to_string(mode));
    require(!error.empty(), "missing aggregate event/reset diagnostic"); return;
  }
  require(succeeded(result), error);
  require(consumed == (mode == 10 || mode == 11 ? 3 : 2) && succeeded(verify(*root)), "aggregate event candidate invalid");
  unsigned registers = 0, masked = 0;
  Value eventRoot = mode == 1 || mode == 2 ? Value() : top.getBodyBlock()->getArgument(1);
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"); name && name == "data" && (mode == 1 || mode == 2)) eventRoot = op->getResult(0);
  });
  top.walk([&](RegOp reg) { ++registers; });
  top.walk([&](NodeOp node) {
    if (!node.getName().starts_with(stem + "_events_0_credit_masked")) return;
    if (occupiedMask) require(node.getName() == stem + "_events_0_credit_masked_0",
      "flattened aggregate leaf must reserve the preferred mask name");
    ++masked;
    auto gate = node.getInput().getDefiningOp<AndPrimOp>();
    require(bool(gate), "aggregate reset mask AND");
    auto negate = gate.getLhs().getDefiningOp<NotPrimOp>();
    require(bool(negate), "aggregate reset mask NOT");
    require(getFieldRefFromValue(gate.getRhs()) == circt::FieldRef(eventRoot, 3) &&
            getFieldRefFromValue(negate.getInput()) == circt::FieldRef(eventRoot, 5),
            "selected aggregate event/reset mask identities mode=" + std::to_string(mode) + " event=" + std::to_string(getFieldRefFromValue(gate.getRhs()).getFieldID()) + " reset=" + std::to_string(getFieldRefFromValue(negate.getInput()).getFieldID()));
  });
  require(registers == 9 && masked == (mode == 10 ? 2 : 1), "aggregate event accounting and Scala flattened mask name");
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "aggregate event annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write aggregate event candidate"); root->print(out); out << '\n';
  }
}

// Source events belong to the child definition; reset masking must happen there
// before the exported event reaches the top accounting domain. A descendant
// sink keeps its synchronizer locally while its enable flows back downward.
void childSources(MLIRContext &context, unsigned mode, StringRef output, unsigned hierarchy = 0, bool childSink = false) {
  std::string payload = "!firrtl.bundle<events: vector<bundle<credit: uint<1>, debit: uint<1>, reset: uint<1>>, 2>>";
  std::string childPorts = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>";
  if (mode == 1) childPorts += ", in %payload: " + payload;
  std::string instancePorts = "in clock: !firrtl.clock, in credit: !firrtl.uint<1>, in debit: !firrtl.uint<1>, in reset: !firrtl.uint<1>";
  if (mode == 1) instancePorts += ", in payload: " + payload;
  childPorts += ", out %echo: !firrtl.uint<1>";
  instancePorts += ", out echo: !firrtl.uint<1>";
  if (childSink) {
    childPorts += ", out %sinkEnabled: !firrtl.uint<1>";
    instancePorts += ", out sinkEnabled: !firrtl.uint<1>";
  }
  auto inst = [&](StringRef name, StringRef module) {
    return "%" + name.str() + ":" + std::to_string((mode == 1 ? 6 : 5) + childSink) +
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
  std::string text = "module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { firrtl.module @Child(" + childPorts + ") {}" + relays + " firrtl.module @Top(in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, in %otherClock: !firrtl.clock, out %enabled: !firrtl.uint<1>, out %echo: !firrtl.uint<1>" + (childSink ? ", out %sinkEnabled: !firrtl.uint<1>" : "") + ") { " + inst(hierarchy ? "relay" : "child", lastModule) + " } } }";
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "parse child source fixture");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto modules = circuit.getOps<FModuleOp>(); auto it = modules.begin();
  auto child = *it++; SmallVector<FModuleOp> relayModules;
  for (unsigned level = 0; level < hierarchy; ++level) relayModules.push_back(*it++);
  auto top = *it;
  unsigned originalPorts = child.getNumPorts(), echoIndex = originalPorts - 1 - childSink;
  auto instance = *top.getBodyBlock()->getOps<InstanceOp>().begin();
  OpBuilder b(&context); auto loc = top.getLoc();
  instance->setAttr("example.metadata", b.getStringAttr("preserve"));
  for (auto relay : relayModules) {
    auto nested = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    nested->setAttr("example.metadata", b.getStringAttr("preserve"));
    b.setInsertionPointToEnd(relay.getBodyBlock());
    for (unsigned i = 0; i < relay.getNumPorts() - 1 - childSink; ++i)
      b.create<StrictConnectOp>(loc, nested.getResult(i), relay.getBodyBlock()->getArgument(i));
    b.create<StrictConnectOp>(loc, relay.getBodyBlock()->getArgument(relay.getNumPorts() - 1),
                              nested.getResult(relay.getNumPorts() - 1));
    if (childSink) b.create<StrictConnectOp>(loc, relay.getBodyBlock()->getArgument(echoIndex), nested.getResult(echoIndex));
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
  b.create<StrictConnectOp>(loc, child.getBodyBlock()->getArgument(echoIndex), creditNode.getResult());
  b.create<NodeOp>(loc, debit, b.getStringAttr("debitEvent"));
  Value sinkClock = child.getBodyBlock()->getArgument(0);
  if (childSink) {
    if (mode == 6) sinkClock = b.create<NodeOp>(loc, sinkClock, b.getStringAttr("sinkClock")).getResult();
    auto one = b.create<ConstantOp>(loc, UIntType::get(&context, 1), llvm::APInt(1, 1));
    auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
    b.create<StrictConnectOp>(loc, child.getBodyBlock()->getArgument(originalPorts - 1), trigger.getResult());
    if (mode == 9) b.create<NodeOp>(loc, sinkClock, b.getStringAttr("sinkClock"));
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  for (unsigned i = 0; i < 4; ++i)
    b.create<StrictConnectOp>(loc, instance.getResult(i), top.getBodyBlock()->getArgument(mode == 3 && i == 0 ? 4 : i));
  if (mode == 1) {
    ImplicitLocOpBuilder fields(loc, b);
    auto zero = b.create<ConstantOp>(loc, UIntType::get(&context, 1), llvm::APInt(1, 0));
    for (auto [id, src] : SmallVector<std::pair<unsigned, Value>>{{3, top.getBodyBlock()->getArgument(1)}, {4, zero}, {5, top.getBodyBlock()->getArgument(3)}, {7, zero}, {8, top.getBodyBlock()->getArgument(2)}, {9, zero}})
      b.create<StrictConnectOp>(loc, getValueByFieldID(fields, instance.getResult(4), id), src);
  }
  b.create<StrictConnectOp>(loc, top.getBodyBlock()->getArgument(6), instance.getResult(echoIndex));
  if (mode == 2 || mode == 10) {
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
  b.create<StrictConnectOp>(loc, top.getBodyBlock()->getArgument(5), childSink ? instance.getResult(originalPorts - 1) : trigger.getResult());
  if (childSink) b.create<StrictConnectOp>(loc, top.getBodyBlock()->getArgument(7), instance.getResult(originalPorts - 1));
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
    a.set("target", ref(mode == 10 ? "Top" : "Child", mode == 10 ? (credit ? "credit" : "debit") : (credit ? "creditEvent" : "debitEvent")));
    a.set("clock", ref(mode == 10 ? "Top" : "Child", mode == 6 ? "clockAlias" : "clock")); a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref(mode == 4 || mode == 10 ? "Top" : "Child", mode == 1 ? "payload.events[0].reset" : "reset"));
    annos.push_back(a.getDictionary(&context));
  }
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)), b.getNamedAttr("target", ref(childSink ? "Child" : "Top", mode == 5 ? "credit" : "trigger")), b.getNamedAttr("clock", ref(childSink ? "Child" : "Top", childSink && (mode == 6 || mode == 9) ? "sinkClock" : "clock"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annos));
  auto before = dump(root.get()); unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode >= 2 && mode != 3 && mode != 6 && mode != 8) {
    require(failed(result) && consumed == 0 && !error.empty() && dump(root.get()) == before,
            "child source failure must preserve module IO, instances and annotations: " + std::to_string(mode)); return;
  }
  require(succeeded(result), error);
  require(consumed == (mode == 8 ? 4 : 2) && succeeded(verify(*root)), "invalid child trigger candidate");
  require(top.getNumPorts() == 7 + childSink && child.getNumPorts() == originalPorts + 2 + childSink, "child event exports changed original IO");
  auto replacement = *top.getBodyBlock()->getOps<InstanceOp>().begin();
  bool echoPreserved = false;
  for (auto connect : top.getBodyBlock()->getOps<StrictConnectOp>())
    if (connect.getDest() == top.getBodyBlock()->getArgument(6))
      echoPreserved = connect.getSrc() == replacement.getResult(echoIndex);
  require(echoPreserved, "existing child output result use lost during replacement");
  require(replacement->getAttrOfType<StringAttr>("example.metadata") == "preserve", "parent instance metadata lost");
  std::string prefix = "child_";
  for (auto relay : relayModules) {
    require(relay.getNumPorts() == child.getNumPorts() + (mode == 8 ? 2 : 0), "relay lost appended event ports");
    for (StringRef event : {"creditEvent_masked", "debitEvent"}) {
      bool found = false;
      for (auto port : relay.getPorts())
        found |= port.getName() == "simulationTrigger_" + prefix + event.str();
      require(found, "Scala intermediate event export names");
    }
    auto nested = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    require(nested->getAttrOfType<StringAttr>("example.metadata") == "preserve", "nested instance metadata lost");
    bool echoConnected = false;
    for (auto connect : relay.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == relay.getBodyBlock()->getArgument(echoIndex))
        echoConnected = connect.getSrc() == nested.getResult(echoIndex);
    require(echoConnected, "relay original output connection lost");
    unsigned relayRegisters = 0, relayMasks = 0;
    relay.walk([&](RegOp) { ++relayRegisters; });
    relay.walk([&](NodeOp node) { if (node.getName() == "creditEvent_masked") ++relayMasks; });
    require(relayRegisters == 0 && relayMasks == 0, "relay duplicated masking or accounting");
    prefix = "middle_" + prefix;
  }
  require(child.getPortName(originalPorts) == "simulationTrigger_creditEvent_masked" &&
          child.getPortName(originalPorts + 1) == "simulationTrigger_debitEvent", "Scala child event export names");
  unsigned registers = 0, masks = 0, childRegisters = 0;
  top.walk([&](RegOp reg) {
    ++registers;
    bool local = reg.getName() == "otherClock_credits" || reg.getName() == "otherClock_debits";
    require(reg.getClockVal() == top.getBodyBlock()->getArgument(mode == 3 && local ? 4 : 0),
            "child trigger local/base accounting clock");
  });
  child.walk([&](RegOp reg) { ++childRegisters; require(reg.getClockVal() == sinkClock, "descendant sink annotated clock lost"); });
  child.walk([&](NodeOp node) { if (node.getName() == "creditEvent_masked") ++masks; });
  require(registers == 9 - childSink && masks == 1 && childRegisters == unsigned(childSink), "child masking/top accounting placement");
  if (childSink) {
    require(child.getPortName(originalPorts + 2) == "trigger_sink", "Scala leaf sink input name");
    for (auto relay : relayModules)
      require(relay.getPortName(originalPorts + 2) == "trigger_source", "Scala relay sink input name");
    bool syncConnected = false;
    for (auto connect : child.getBodyBlock()->getOps<StrictConnectOp>())
      if (auto reg = connect.getDest().getDefiningOp<RegOp>())
        syncConnected = reg.getName() == "trigger_sync" && connect.getSrc() == child.getBodyBlock()->getArgument(originalPorts + 2);
    require(syncConnected, "descendant synchronizer is not driven by appended sink input");
  }
  require(cast<ArrayAttr>(circuit->getAttr("rawAnnotations")).size() == 1, "child annotation cleanup");
  if (!output.empty()) { std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, "write child source candidate"); root->print(out); out << '\n'; }
}

// Two child sinks and an ancestor sink share one downward enable route. Scala
// chooses the last annotation per node, then emits synchronizers in statement
// order. Distinct local clock aliases make the naming/identity contract visible.
void sharedSinks(MLIRContext &context, unsigned mode, StringRef output) {
  std::string ports = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, "
    "in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, "
    "out %echo: !firrtl.uint<1>, out %sinkEnabled: !firrtl.uint<1>, "
    "out %secondEnabled: !firrtl.uint<1>, out %relayEnabled: !firrtl.uint<1>";
  std::string instancePorts = ports;
  instancePorts.erase(std::remove(instancePorts.begin(), instancePorts.end(), '%'), instancePorts.end());
  auto instanceText = [&](StringRef name, StringRef module) {
    return "%" + name.str() + ":8 = firrtl.instance " + name.str() + " @" +
      module.str() + "(" + instancePorts + ")";
  };
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes "
    "{rawAnnotations = []} { firrtl.module @Child(" + ports + ") {} "
    "firrtl.module @Relay(" + ports + ") { " + instanceText("child", "Child") + " } "
    "firrtl.module @Top(" + ports + ") { " + instanceText("relay", "Relay") + " } } }", &context);
  require(bool(root), "parse shared sinks");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto it = circuit.getOps<FModuleOp>().begin();
  auto child = *it++, relay = *it++, top = *it;
  OpBuilder b(&context); auto loc = top.getLoc();
  auto arg = [](FModuleOp module, unsigned index) { return module.getBodyBlock()->getArgument(index); };
  b.setInsertionPointToEnd(child.getBodyBlock());
  auto bit = UIntType::get(&context, 1);
  auto zero = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 0));
  auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
  auto credit = b.create<NodeOp>(loc, arg(child, 1), b.getStringAttr("creditEvent"));
  b.create<NodeOp>(loc, arg(child, 2), b.getStringAttr("debitEvent"));
  auto clockA = b.create<NodeOp>(loc, arg(child, 0), b.getStringAttr("clockA"));
  auto clockB = b.create<NodeOp>(loc, arg(child, 0), b.getStringAttr("clockB"));
  if (mode == 2) b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger_sync"));
  auto sinkA = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("triggerA"));
  auto sinkB = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("triggerB"));
  b.create<StrictConnectOp>(loc, arg(child, 4), credit.getResult());
  b.create<StrictConnectOp>(loc, arg(child, 5), sinkA.getResult());
  b.create<StrictConnectOp>(loc, arg(child, 6), sinkB.getResult());
  b.create<StrictConnectOp>(loc, arg(child, 7), zero.getResult());
  NodeOp sinkR, relayClock;
  for (auto module : {relay, top}) {
    b.setInsertionPointToEnd(module.getBodyBlock());
    auto instance = *module.getBodyBlock()->getOps<InstanceOp>().begin();
    instance->setAttr("example.metadata", b.getStringAttr("preserve"));
    for (unsigned i = 0; i < 4; ++i)
      b.create<StrictConnectOp>(loc, instance.getResult(i), arg(module, i));
    for (unsigned i = 4; i < 8; ++i) {
      Value signal = instance.getResult(i);
      if (module == relay && i == 7) {
        relayClock = b.create<NodeOp>(loc, arg(relay, 0), b.getStringAttr("relayClock"));
        auto relayOne = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
        sinkR = b.create<NodeOp>(loc, relayOne.getResult(), b.getStringAttr("triggerR"));
        signal = sinkR.getResult();
      }
      b.create<StrictConnectOp>(loc, arg(module, i), signal);
    }
  }
  auto ref = [&](StringRef module, StringRef name) { return b.getStringAttr(("~Top|" + module + ">" + name).str()); };
  auto sink = [&](StringRef module, StringRef name, StringRef clock) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref(module, name)), b.getNamedAttr("clock", ref(module, clock))});
  };
  SmallVector<Attribute> annotations;
  for (bool credit : {true, false}) {
    NamedAttrList attrs;
    attrs.set("class", b.getStringAttr(A::InternalTriggerSource));
    attrs.set("target", ref("Child", credit ? "creditEvent" : "debitEvent"));
    attrs.set("clock", ref("Child", "clock")); attrs.set("sourceType", b.getBoolAttr(credit));
    if (credit) attrs.set("reset", ref("Child", "reset"));
    annotations.push_back(attrs.getDictionary(&context));
  }
  annotations.push_back(sink("Child", "triggerB", "clockB"));
  if (mode == 1) annotations.push_back(sink("Child", "triggerA", "clockB"));
  annotations.push_back(sink("Child", "triggerA", "clockA"));
  annotations.push_back(sink("Relay", "triggerR", "relayClock"));
  if (mode == 3) annotations.push_back(sink("Child", "triggerA", "missingClock"));
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("Top", "clock")}))});
  annotations.push_back(channel);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(root.get()); std::string error; unsigned consumed = 99;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode == 3) {
    require(failed(result) && consumed == 0 && before == dump(root.get()) && !error.empty(),
            "last duplicate sink must be validated atomically");
    return;
  }
  require(succeeded(result), error);
  require(consumed == 2 && succeeded(verify(*root)), "invalid shared sink IR");
  require(top.getNumPorts() == 8 && child.getNumPorts() == 11 && relay.getNumPorts() == 11,
          "shared sinks must append exactly one enable input per module");
  require(child.getPortName(10) == "trigger_sink" && relay.getPortName(10) == "trigger_sink",
          "ancestor with a local sink must use the sink wiring key");
  auto check = [&](NodeOp node, Value clock, StringRef name) {
    auto reg = node.getInput().getDefiningOp<RegOp>();
    require(reg && reg.getName() == name && reg.getClockVal() == clock,
            "sink declaration order / last annotation clock mismatch: " + node.getName().str());
    auto module = node->getParentOfType<FModuleOp>();
    unsigned drivers = 0;
    for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == reg.getResult()) {
        ++drivers; require(connect.getSrc() == arg(module, 10), "sink enable is not shared");
      }
    require(drivers == 1, "sink needs one synchronizer driver");
  };
  check(sinkA, clockA.getResult(), mode == 2 ? "trigger_sync_0" : "trigger_sync");
  check(sinkB, clockB.getResult(), mode == 2 ? "trigger_sync_1" : "trigger_sync_0");
  check(sinkR, relayClock.getResult(), "trigger_sync");
  for (auto module : {relay, top}) {
    auto instance = *module.getBodyBlock()->getOps<InstanceOp>().begin();
    Value expected = module == relay ? arg(relay, 10) : Value();
    if (module == top)
      for (auto node : top.getBodyBlock()->getOps<NodeOp>())
        if (node.getName() == "trigger_source") expected = node.getResult();
    require(instance.getNumResults() == 11 &&
      instance->getAttrOfType<StringAttr>("example.metadata").getValue() == "preserve",
      "shared route replaced instance metadata or ports");
    unsigned drivers = 0;
    for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == instance.getResult(10)) {
        ++drivers; require(expected && connect.getSrc() == expected, "wrong shared sink route");
      }
    require(drivers == 1, "shared sink route needs one driver");
  }
  unsigned registers = 0; circuit.walk([&](RegOp) { ++registers; });
  require(registers == 11 && circuit->getAttr("rawAnnotations") == b.getArrayAttr({channel}),
          "shared sink register count / annotation cleanup");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write shared sink candidate"); root->print(out); out << '\n';
  }
}

// Pathless sink annotations fan out across siblings, distinct parents and
// repeated ancestors, with independent state per instance. Unequal route depths
// must put relay inputs before leaf inputs; source exports replace ancestors first.
void fanoutSinks(MLIRContext &context, unsigned mode, StringRef output) {
  std::string leafPorts = "in %clock: !firrtl.clock, in %data: !firrtl.uint<1>, "
    "out %first: !firrtl.uint<1>, out %second: !firrtl.uint<1>, out %echo: !firrtl.uint<1>";
  std::string ports = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, "
    "in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, in %otherClock: !firrtl.clock, "
    "out %first0: !firrtl.uint<1>, out %second0: !firrtl.uint<1>, "
    "out %first1: !firrtl.uint<1>, out %second1: !firrtl.uint<1>, "
    "out %echo0: !firrtl.uint<1>, out %echo1: !firrtl.uint<1>";
  auto instanceText = [&](StringRef name, StringRef module, std::string interface, unsigned count) {
    interface.erase(std::remove(interface.begin(), interface.end(), '%'), interface.end());
    return "%" + name.str() + ":" + std::to_string(count) + " = firrtl.instance " +
      name.str() + " @" + module.str() + "(" + interface + ")";
  };
  auto left = instanceText("left", "Leaf", leafPorts, 5);
  auto right = instanceText("right", "Leaf", leafPorts, 5);
  bool split = mode == 3 || mode == 6 || mode == 7 || mode == 8;
  bool repeated = mode == 4 || mode >= 9;
  bool withOuter = mode == 9 || mode == 12;
  bool hasRelay = mode == 1 || repeated || split;
  auto relayText = hasRelay ? "firrtl.module @Relay(" + ports + ") { " +
    (mode == 1 || repeated ? left + " " : "") + right + " } " : "";
  auto topText = hasRelay ? (split ? left + " " : "") +
    instanceText("relay", "Relay", ports, 11) : left + " " + right;
  auto repeatedRelays = instanceText("relay", "Relay", ports, 11) + " " +
    instanceText("otherRelay", "Relay", ports, 11);
  auto outerText = withOuter ? "firrtl.module @Outer(" + ports + ") { " + repeatedRelays + " } " : "";
  if (withOuter) topText = instanceText("outer", "Outer", ports, 11);
  else if (repeated) topText = repeatedRelays;
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes "
    "{rawAnnotations = []} { firrtl.module @Leaf(" + leafPorts + ") {} " +
    relayText + outerText + "firrtl.module @Top(" + ports + ") { " + topText + " } } }", &context);
  require(bool(root), "parse sink fanout");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp leaf, relay, outer, top;
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module.getName() == "Leaf") leaf = module;
    else if (module.getName() == "Relay") relay = module;
    else if (module.getName() == "Outer") outer = module;
    else top = module;
  }
  OpBuilder b(&context); auto loc = top.getLoc();
  auto arg = [](FModuleOp module, unsigned index) { return module.getBodyBlock()->getArgument(index); };
  b.setInsertionPointToEnd(leaf.getBodyBlock());
  auto clockAlias = b.create<NodeOp>(loc, arg(leaf, 0), b.getStringAttr("clockAlias"));
  auto bit = UIntType::get(&context, 1);
  auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
  auto sinkA = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("triggerA"));
  auto sinkB = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("triggerB"));
  b.create<StrictConnectOp>(loc, arg(leaf, 2), sinkA.getResult());
  b.create<StrictConnectOp>(loc, arg(leaf, 3), sinkB.getResult());
  b.create<StrictConnectOp>(loc, arg(leaf, 4), arg(leaf, 1));
  for (auto module : circuit.getOps<FModuleOp>()) {
    if (module == leaf) continue;
    b.setInsertionPointToEnd(module.getBodyBlock());
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      instance->setAttr("example.metadata", b.getStringAttr("preserve"));
      if (instance.getModuleName() == "Relay" || instance.getModuleName() == "Outer") {
        bool wrongClock = mode == 7 ||
          ((mode == 10 || mode == 12) && instance.getName() == "otherRelay") ||
          (mode == 11 && instance.getName() == "relay");
        for (unsigned i = 0; i < 5; ++i)
          b.create<StrictConnectOp>(loc, instance.getResult(i), arg(module, wrongClock && i == 0 ? 4 : i));
        if (instance.getName() == "relay" || instance.getName() == "outer")
          for (unsigned i = 5; i < 11; ++i)
            if (!split || i == 7 || i == 8 || i == 10)
              b.create<StrictConnectOp>(loc, arg(module, i), instance.getResult(i));
      } else {
        bool isLeft = instance.getName() == "left";
        // Modes 14/15 leave one absolute sink path unproven: missing clock
        // or multiple unconditional drivers. All independent roots are valid.
        if (!(mode == 14 && module == relay && !isLeft))
          b.create<StrictConnectOp>(loc, instance.getResult(0), arg(module, ((mode == 2 && !isLeft) || ((mode == 5 || mode == 8) && isLeft)) ? 4 : 0));
        if (mode == 15 && module == relay && !isLeft)
          b.create<StrictConnectOp>(loc, instance.getResult(0), arg(module, 4));
        b.create<StrictConnectOp>(loc, instance.getResult(1), arg(module, isLeft ? 1 : 2));
        b.create<StrictConnectOp>(loc, arg(module, isLeft ? 5 : 7), instance.getResult(2));
        b.create<StrictConnectOp>(loc, arg(module, isLeft ? 6 : 8), instance.getResult(3));
        b.create<StrictConnectOp>(loc, arg(module, isLeft ? 9 : 10), instance.getResult(4));
      }
    }
  }
  if (split) {
    b.setInsertionPointToEnd(relay.getBodyBlock());
    auto zero = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 0));
    for (unsigned i : {5, 6, 9}) b.create<StrictConnectOp>(loc, arg(relay, i), zero.getResult());
  }
  auto sourceModule = mode == 9 ? outer : (mode == 1 || mode == 6 ? relay : top);
  b.setInsertionPointToEnd(sourceModule.getBodyBlock());
  b.create<NodeOp>(loc, arg(sourceModule, 1), b.getStringAttr("creditEvent"));
  b.create<NodeOp>(loc, arg(sourceModule, 2), b.getStringAttr("debitEvent"));
  auto ref = [&](StringRef module, StringRef name) { return b.getStringAttr(("~Top|" + module + ">" + name).str()); };
  SmallVector<Attribute> annotations;
  for (bool credit : {true, false}) {
    NamedAttrList attrs; attrs.set("class", b.getStringAttr(A::InternalTriggerSource));
    attrs.set("target", ref(sourceModule.getName(), credit ? "creditEvent" : "debitEvent"));
    attrs.set("clock", ref(sourceModule.getName(), "clock")); attrs.set("sourceType", b.getBoolAttr(credit));
    if (credit) attrs.set("reset", ref(sourceModule.getName(), "reset"));
    annotations.push_back(attrs.getDictionary(&context));
  }
  for (StringRef name : {"triggerB", "triggerA"})
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref("Leaf", name)), b.getNamedAttr("clock", ref("Leaf", "clockAlias"))}));
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("Top", "clock")}))});
  annotations.push_back(channel); circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (mode == 13) {
    // The parser rejects direct recursion. Inject it afterwards to exercise
    // this pass's preflight rejection without changing the input IR.
    b.setInsertionPointToEnd(relay.getBodyBlock());
    auto exemplar = *top.getBodyBlock()->getOps<InstanceOp>().begin();
    auto cycle = cast<InstanceOp>(b.clone(*exemplar.getOperation()));
    cycle.setNameAttr(b.getStringAttr("cycle"));
  }
  auto before = dump(root.get()); unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode >= 13) {
    require(failed(result) && consumed == 0 && before == dump(root.get()) && !error.empty(),
            "unsupported sink fanout must fail atomically");
    if (mode == 13) require(StringRef(error).contains("cycle"), "sink cycle was not diagnosed");
    else require(StringRef(error).contains("every instance path"),
                 "unproven sink instance clock was not diagnosed");
    return;
  }
  require(succeeded(result), error);
  require(consumed == 2 && succeeded(verify(*root)), "invalid sink fanout IR");
  require(top.getNumPorts() == 11 && leaf.getNumPorts() == 6 && leaf.getPortName(5) == "trigger_sink",
          "sink fanout must append one module input, preserving top IO");
  unsigned relayEnable = sourceModule == relay ? 13 : 11;
  if (relay) require(relay.getNumPorts() == relayEnable + 1 && relay.getPortName(relayEnable) == "trigger_source",
                     "relay lost original ports, source exports or enable input");
  unsigned outerEnable = sourceModule == outer ? 13 : 11;
  if (outer) require(outer.getNumPorts() == outerEnable + 1 && outer.getPortName(outerEnable) == "trigger_source",
                     "outer lost original ports, source exports or enable input");
  for (auto [node, name] : {std::pair{sinkA, StringRef("trigger_sync")},
                            std::pair{sinkB, StringRef("trigger_sync_0")}}) {
    auto reg = node.getInput().getDefiningOp<RegOp>();
    require(reg && reg.getName() == name && reg.getClockVal() == clockAlias.getResult(),
            "fanout synchronizers must follow sink declaration order");
  }
  Value enable;
  for (auto node : top.getBodyBlock()->getOps<NodeOp>())
    if (node.getName() == "trigger_source") enable = node.getResult();
  unsigned leafInstances = 0, registers = 0;
  circuit.walk([&](RegOp) { ++registers; });
  for (auto module : circuit.getOps<FModuleOp>())
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      require(instance->getAttrOfType<StringAttr>("example.metadata").getValue() == "preserve",
              "sink fanout discarded instance metadata");
      if (instance.getModuleName() != "Leaf") {
        unsigned enableIndex = instance.getModuleName() == "Relay" ? relayEnable : outerEnable;
        require(instance.getNumResults() == enableIndex + 1, "relay instance lost appended ports");
        unsigned drivers = 0, originalDrivers = 0;
        for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>()) {
          if (connect.getDest() == instance.getResult(enableIndex)) {
            ++drivers;
            require(connect.getSrc() == (module == top ? enable : arg(outer, outerEnable)),
                    "each ancestor instance must receive the trigger enable");
          }
          for (unsigned i = 0; i < 5; ++i)
            originalDrivers += connect.getDest() == instance.getResult(i);
        }
        require(drivers == 1 && originalDrivers == 5, "ancestor lost or duplicated input connections");
        continue;
      }
      ++leafInstances; require(instance.getNumResults() == 6, "each sink instance needs appended enable port");
      unsigned drivers = 0, originalDrivers = 0;
      for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>()) {
        if (connect.getDest() == instance.getResult(5)) {
          ++drivers; require(connect.getSrc() == (module == top ? enable : arg(module, relayEnable)),
                             "each sink instance must receive the same trigger enable");
        }
        if (connect.getDest() == instance.getResult(0) || connect.getDest() == instance.getResult(1))
          ++originalDrivers;
      }
      require(drivers == 1 && originalDrivers == 2, "fanout lost or duplicated input connections");
    }
  require(leafInstances == 2 && registers == 10 && circuit->getAttr("rawAnnotations") == b.getArrayAttr({channel}),
          "fanout register definitions / annotation cleanup mismatch");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write fanout candidate"); root->print(out); out << '\n';
  }
}

// Each pathless source contributes once per complete instance path. Keep masking
// in the child definition so repeated ancestors have independent reset inputs.
void fanoutSources(MLIRContext &context, unsigned mode, StringRef output, unsigned relayDepth = 0,
                   bool sharedTarget = false, bool mixedMasks = false, bool collidingMasks = false,
                   unsigned ancestorCollision = 0, unsigned topCollision = 0, bool maskContainer = false, unsigned exportLeaf = 0) {
  bool lastSourceClock = mode >= 13;
  bool childSink = mode >= 14 || mode == 1 || mode == 5 || mode == 6 || mode == 7 || mode == 10 || mode == 12;
  bool duplicateMasked = mode == 11 || mode == 12;
  unsigned exports = mixedMasks ? 3 : sharedTarget ? 1 : duplicateMasked ? 3 : 2;
  std::string childPorts = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, "
    "in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, out %echo: !firrtl.uint<1>";
  if (childSink) childPorts += ", out %sinkEnabled: !firrtl.uint<1>";
  if (lastSourceClock) childPorts += ", in %alternateClock: !firrtl.clock";
  std::string instancePorts = childPorts;
  instancePorts.erase(std::remove(instancePorts.begin(), instancePorts.end(), '%'), instancePorts.end());
  unsigned originalPorts = (childSink ? 6 : 5) + lastSourceClock;
  std::string creditExport = lastSourceClock && !mixedMasks && !collidingMasks ? "creditEvent" : "creditEvent_masked";
  std::string finalExport = collidingMasks ? "simulationTrigger_creditEvent_masked" : mixedMasks ? "creditEvent_masked_0" : sharedTarget ? "creditEvent" : "debitEvent";
  auto inst = [&](StringRef name, StringRef module) { return "%" + name.str() + ":" + std::to_string(originalPorts) +
    " = firrtl.instance " + name.str() + " @" + module.str() + "(" + instancePorts + ")"; };
  bool vectorCollision = topCollision == 8 || topCollision == 9;
  bool aggregateCollision = topCollision >= 6;
  std::string relayChild = vectorCollision ? "child_0" : "child";
  std::string aggregateType = "!firrtl.bundle<child: " +
      std::string(vectorCollision ? "vector<bundle<creditEvent_masked: uint<1>>, 1>" : "bundle<creditEvent_masked: uint<1>>") + ">";
  if (topCollision == 10) aggregateType = "!firrtl.bundle<neighbor: uint<1>>";
  std::string relayText;
  for (unsigned depth = 0; depth < relayDepth; ++depth)
    relayText += "firrtl.module @" + std::string(depth ? "Outer" : "Relay") + "(" + childPorts +
      ") { " + inst(depth ? "relay" : relayChild, depth ? "Relay" : "Child") + " } ";
  StringRef repeatedModule = relayDepth == 2 ? "Outer" : relayDepth == 1 ? "Relay" : "Child";
  std::string topExport = "simulationTrigger_first_" +
      std::string(relayDepth == 2 ? "relay_child_" : relayDepth == 1 ?
                  vectorCollision ? "child_0_" : "child_" : "") + "creditEvent_masked";
  std::string collisionPort = topCollision == 3 ? ", in %" + topExport + ": !firrtl.uint<1>" :
      topCollision == 6 || topCollision == 8 ? ", in %simulationTrigger_first: " + aggregateType :
      topCollision == 10 ? ", in %" + topExport + ": " + aggregateType : "";
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { "
    "firrtl.module @Child(" + childPorts + ") {} " + relayText + "firrtl.module @Top(in %clock: !firrtl.clock, "
    "in %credit0: !firrtl.uint<1>, in %debit0: !firrtl.uint<1>, in %reset0: !firrtl.uint<1>, "
    "in %credit1: !firrtl.uint<1>, in %debit1: !firrtl.uint<1>, in %reset1: !firrtl.uint<1>, "
    "in %otherClock: !firrtl.clock, out %enabled: !firrtl.uint<1>, out %echo0: !firrtl.uint<1>, "
    "out %echo1: !firrtl.uint<1>" + (childSink ? std::string(", out %sink0: !firrtl.uint<1>, out %sink1: !firrtl.uint<1>") : "") +
    collisionPort + ") { " + inst("first", repeatedModule) + " " + inst("second", repeatedModule) + " } } }", &context);
  require(bool(root), "parse source fanout");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto it = circuit.getOps<FModuleOp>().begin(); auto child = *it++;
  SmallVector<FModuleOp> relays;
  for (unsigned depth = 0; depth < relayDepth; ++depth) relays.push_back(*it++);
  auto top = *it;
  OpBuilder b(&context); auto loc = top.getLoc(); auto bit = UIntType::get(&context, 1);
  auto arg = [](FModuleOp m, unsigned i) { return m.getBodyBlock()->getArgument(i); };
  b.setInsertionPointToEnd(child.getBodyBlock());
  if (maskContainer) {
    auto type = parseType("!firrtl.bundle<neighbor: uint<1>>", &context);
    auto wire = b.create<WireOp>(loc, type, "creditEvent_masked");
    auto invalid = b.create<InvalidValueOp>(loc, type);
    b.create<StrictConnectOp>(loc, wire.getResult(), invalid.getResult());
  }
  auto reserveExportLeaf = [&](FModuleOp module, StringRef name, bool node) {
    b.setInsertionPointToEnd(module.getBodyBlock());
    auto type = parseType(exportLeaf >= 4 ? "!firrtl.bundle<neighbor: uint<1>>" :
                         "!firrtl.bundle<masked: uint<1>>", &context);
    auto invalid = b.create<InvalidValueOp>(loc, type);
    if (node) b.create<NodeOp>(loc, invalid.getResult(), b.getStringAttr(name));
    else {
      auto wire = b.create<WireOp>(loc, type, name);
      b.create<StrictConnectOp>(loc, wire.getResult(), invalid.getResult());
    }
  };
  if (exportLeaf == 1) reserveExportLeaf(child, "simulationTrigger_simulationTrigger_creditEvent", false);
  if (exportLeaf == 4)
    reserveExportLeaf(child, "simulationTrigger_simulationTrigger_creditEvent_masked", false);
  auto credit = b.create<NodeOp>(loc, arg(child, 1), b.getStringAttr("creditEvent"));
  auto debit = b.create<NodeOp>(loc, arg(child, 2),
      b.getStringAttr(collidingMasks ? "simulationTrigger_creditEvent" : "debitEvent"));
  b.create<StrictConnectOp>(loc, arg(child, 4), credit.getResult());
  if (childSink) {
    auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
    auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
    b.create<StrictConnectOp>(loc, arg(child, 5), trigger.getResult());
  }
  for (auto relay : relays) {
    b.setInsertionPointToEnd(relay.getBodyBlock());
    if (exportLeaf && exportLeaf != 1 && exportLeaf != 4 && relay == relays.front())
      reserveExportLeaf(relay, exportLeaf >= 5 ? "simulationTrigger_child_creditEvent_masked" :
                        "simulationTrigger_child_creditEvent", exportLeaf == 3 || exportLeaf == 6);
    if (ancestorCollision && relay == relays.front())
      b.create<NodeOp>(loc, arg(relay, 2), b.getStringAttr(ancestorCollision == 1 ?
        "simulationTrigger_child_creditEvent" : "child_creditEvent_masked"));
    auto instance = *relay.getBodyBlock()->getOps<InstanceOp>().begin();
    instance->setAttr("example.metadata", b.getStringAttr("preserve"));
    for (unsigned i = 0; i < 4; ++i)
      b.create<StrictConnectOp>(loc, instance.getResult(i), arg(relay, i));
    if (lastSourceClock)
      b.create<StrictConnectOp>(loc, instance.getResult(originalPorts - 1), arg(relay, originalPorts - 1));
    for (unsigned i = 4; i < originalPorts - lastSourceClock; ++i)
      b.create<StrictConnectOp>(loc, arg(relay, i), instance.getResult(i));
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  if (topCollision && topCollision != 3 && topCollision != 6 && topCollision != 8 && topCollision != 10) {
    std::string collisionName = topCollision == 4 ? topExport.substr(0, topExport.size() - 7) :
                                topCollision == 5 ? topExport + "_neighbor" : topExport;
    if (aggregateCollision) {
      auto type = parseType(aggregateType, &context);
      require(bool(type), "parse aggregate top collision type");
      auto wire = b.create<WireOp>(loc, type, "simulationTrigger_first");
      auto invalid = b.create<InvalidValueOp>(loc, type);
      b.create<StrictConnectOp>(loc, wire.getResult(), invalid.getResult());
    } else if (topCollision == 2) {
      auto wire = b.create<WireOp>(loc, bit, collisionName);
      b.create<StrictConnectOp>(loc, wire.getResult(), arg(top, 2));
    } else b.create<NodeOp>(loc, arg(top, 2), b.getStringAttr(collisionName));
  }
  for (auto [index, instance] : llvm::enumerate(top.getBodyBlock()->getOps<InstanceOp>())) {
    instance->setAttr("example.metadata", b.getStringAttr("preserve"));
    bool wrongClock = mode == 4 || mode == 5 ||
                      ((mode == 2 || mode == 6 || mode >= 9) && index == 1) ||
                      ((mode == 3 || mode == 7) && index == 0);
    if (!(mode == 8 && index == 1))
      b.create<StrictConnectOp>(loc, instance.getResult(0), arg(top, wrongClock ? 7 : 0));
    // The alternate clock is swapped between absolute instances. Modes 15/16
    // omit one alias: only a selected clock may constrain source preflight.
    if (lastSourceClock && !((mode == 15 || mode == 16) && index == 1))
      b.create<StrictConnectOp>(loc, instance.getResult(originalPorts - 1), arg(top, index ? 0 : 7));
    for (unsigned i = 1; i <= 3; ++i)
      b.create<StrictConnectOp>(loc, instance.getResult(i), arg(top, i + 3 * index));
    b.create<StrictConnectOp>(loc, arg(top, 9 + index), instance.getResult(4));
    if (childSink) b.create<StrictConnectOp>(loc, arg(top, 11 + index), instance.getResult(5));
  }
  auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
  auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
  b.create<StrictConnectOp>(loc, arg(top, 8), trigger.getResult());
  auto ref = [&](StringRef module, StringRef name) { return b.getStringAttr(("~Top|" + module + ">" + name).str()); };
  SmallVector<Attribute> annotations;
  for (bool credit : {!sharedTarget, sharedTarget}) {
    NamedAttrList attrs; attrs.set("class", b.getStringAttr(A::InternalTriggerSource));
    attrs.set("target", ref("Child", credit || sharedTarget ? "creditEvent" : "debitEvent"));
    attrs.set("clock", ref("Child", "clock")); attrs.set("sourceType", b.getBoolAttr(credit));
    if (credit && !lastSourceClock) attrs.set("reset", ref("Child", "reset"));
    if (lastSourceClock && ((mode == 15 || (mode == 14 && relayDepth)) != (sharedTarget && credit)))
      attrs.set("clock", ref("Child", "alternateClock"));
    annotations.push_back(attrs.getDictionary(&context));
    if (lastSourceClock) {
      attrs.set("clock", ref("Child", (mode == 15 || (mode == 14 && relayDepth)) != (sharedTarget && credit) ? "clock" : "alternateClock"));
      attrs.set("class", b.getStringAttr(A::TriggerSource));
      annotations.push_back(attrs.getDictionary(&context));
    }
    if (credit && duplicateMasked) annotations.push_back(attrs.getDictionary(&context));
    // An unmasked duplicate shares every absolute export. Exercise both raw
    // class spellings; consumption still counts annotations, not unique events.
    if (!credit && (mode == 9 || mode == 10)) {
      annotations.push_back(attrs.getDictionary(&context));
      attrs.set("class", b.getStringAttr(A::TriggerSource));
      annotations.push_back(attrs.getDictionary(&context));
    }
  }
  if (mixedMasks) {
    // Deliberately put masked debits before masked credits on the same target.
    // Scala creates credit masks first; unmasked membership is shared, while
    // the two distinct reset inputs and clocks remain independent.
    annotations.clear();
    for (bool isCredit : {false, true}) {
      NamedAttrList attrs;
      attrs.set("class", b.getStringAttr(isCredit ? A::TriggerSource : A::InternalTriggerSource));
      attrs.set("target", ref("Child", "creditEvent"));
      attrs.set("clock", ref("Child", isCredit ? "clock" : "alternateClock"));
      attrs.set("sourceType", b.getBoolAttr(isCredit));
      attrs.set("reset", ref("Child", isCredit ? "reset" : "debit"));
      annotations.push_back(attrs.getDictionary(&context));
      attrs.erase("reset"); attrs.set("clock", ref("Child", "clock"));
      annotations.push_back(attrs.getDictionary(&context));
    }
  }
  if (collidingMasks) {
    // The debit mask occupies the credit export's preferred name. Scala
    // creates every mask before TopWiring allocates ports, including relays.
    annotations.clear();
    for (bool isCredit : {false, true})
      annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSource)),
        b.getNamedAttr("target", ref("Child", isCredit ? "creditEvent" : "simulationTrigger_creditEvent")),
        b.getNamedAttr("clock", ref("Child", "clock")),
        b.getNamedAttr("reset", ref("Child", isCredit ? "reset" : "credit")),
        b.getNamedAttr("sourceType", b.getBoolAttr(isCredit))}));
  }
  if (ancestorCollision) {
    NamedAttrList attrs;
    attrs.set("class", b.getStringAttr(A::InternalTriggerSource));
    attrs.set("target", ref("Relay", ancestorCollision == 1 ?
      "simulationTrigger_child_creditEvent" : "child_creditEvent_masked"));
    attrs.set("clock", ref("Relay", "clock"));
    attrs.set("sourceType", b.getBoolAttr(false));
    if (ancestorCollision == 1) attrs.set("reset", ref("Relay", "reset"));
    annotations.push_back(attrs.getDictionary(&context));
  }
  if (topCollision == 4)
    annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSource)),
      b.getNamedAttr("target", ref("Top", topExport.substr(0, topExport.size() - 7))),
      b.getNamedAttr("clock", ref("Top", "clock")),
      b.getNamedAttr("reset", ref("Top", "reset0")),
      b.getNamedAttr("sourceType", b.getBoolAttr(false))}));
  SmallVector<StringRef> sinkModules{"Top"};
  if (childSink) sinkModules.push_back("Child");
  for (StringRef module : sinkModules)
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref(module, "trigger")), b.getNamedAttr("clock", ref(module, "clock"))}));
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("Top", "clock")}))});
  annotations.push_back(channel); circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (topCollision) require(succeeded(verify(*root)), "top collision input must be valid FIRRTL IR");
  auto before = dump(root.get()); unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  if (mode == 8 || mode == 16 || ancestorCollision == 2 || (topCollision && topCollision != 5 && topCollision != 10)) {
    require(failed(result) && consumed == 0 && before == dump(root.get()) && !error.empty(),
            "source fanout unsupported path must fail atomically: " + error);
    if (topCollision) require(error == "top declaration collides with trigger export: " + topExport,
      "top export collision needs a specific diagnostic");
    if (ancestorCollision == 2) require(error.find("ambiguous flattened trigger source") != std::string::npos,
      "flattened ancestor/descendant source collision needs a specific diagnostic");
    return;
  }
  require(succeeded(result), error);
  require(consumed == (collidingMasks ? 2 + bool(ancestorCollision) : duplicateMasked ? 3 : mode >= 9 ? 4 : 2) && succeeded(verify(*root)), "invalid source fanout IR");
  require(top.getNumPorts() == 11 + 2 * childSink + (topCollision == 10) && child.getNumPorts() == originalPorts + exports + childSink,
          "source fanout must preserve top IO and share child export definitions");
  require(child.getPortName(originalPorts) == "simulationTrigger_" + creditExport +
              (collidingMasks ? "_0" : "") &&
          child.getPortName(originalPorts + exports - 1) ==
              "simulationTrigger_" + finalExport + (exportLeaf == 1 ? "_0" : "") &&
          (!duplicateMasked || child.getPortName(originalPorts + 1) == "simulationTrigger_creditEvent_masked_0"), "source fanout export names");
  std::string prefix;
  for (auto [depth, relay] : llvm::enumerate(relays)) {
    prefix = std::string(depth ? "relay_" : "child_") + prefix;
    require(relay.getNumPorts() == originalPorts + exports + bool(ancestorCollision) + childSink &&
            relay.getPortName(originalPorts) == "simulationTrigger_" + prefix + creditExport +
              ((ancestorCollision == 1 || exportLeaf == 2 || exportLeaf == 3) && !depth ? "_0" : "") &&
            relay.getPortName(originalPorts + exports - 1) == "simulationTrigger_" + prefix +
                finalExport &&
            (!duplicateMasked || relay.getPortName(originalPorts + 1) == "simulationTrigger_" + prefix + "creditEvent_masked_0"),
            "ancestor must relay each event through one export per definition");
  }
  if (ancestorCollision == 1) {
    unsigned ancestorMasks = 0;
    auto relay = relays.front();
    relay.walk([&](NodeOp node) {
      if (node.getName() != "simulationTrigger_child_creditEvent_masked") return;
      auto gate = node.getInput().getDefiningOp<AndPrimOp>();
      auto negate = gate ? gate.getLhs().getDefiningOp<NotPrimOp>() : NotPrimOp();
      auto event = gate ? gate.getRhs().getDefiningOp<NodeOp>() : NodeOp();
      require(negate && event && negate.getInput() == arg(relay, 3) &&
              event.getName() == "simulationTrigger_child_creditEvent" &&
              event.getInput() == arg(relay, 2), "ancestor mask lost its local event/reset driver");
      ++ancestorMasks;
    });
    require(ancestorMasks == 1 && relay.getPortName(originalPorts + exports) ==
            "simulationTrigger_simulationTrigger_child_creditEvent_masked",
            "ancestor source must retain its distinct mask/export identity");
    unsigned exportDrivers = 0;
    for (auto connect : relay.getBodyBlock()->getOps<StrictConnectOp>()) {
      if (connect.getDest() == arg(relay, originalPorts)) {
        auto result = dyn_cast<OpResult>(connect.getSrc());
        auto instance = result ? dyn_cast<InstanceOp>(result.getOwner()) : InstanceOp();
        require(instance && instance.getName() == "child" &&
                instance.getPortName(result.getResultNumber()) == "simulationTrigger_creditEvent_masked_0",
                "renamed descendant relay export must retain its child's masked source");
        ++exportDrivers;
      }
    }
    require(exportDrivers == 1, "descendant relay export needs one driver");
  }
  unsigned instances = 0, masks = 0, registers = 0;
  child.walk([&](NodeOp n) {
    if (!n.getName().starts_with("creditEvent_masked")) return;
    ++masks;
    if (mixedMasks || collidingMasks) {
      auto gate = n.getInput().getDefiningOp<AndPrimOp>();
      auto negate = gate ? gate.getLhs().getDefiningOp<NotPrimOp>() : NotPrimOp();
      require(gate && negate && gate.getRhs() == credit.getResult() &&
              negate.getInput() == arg(child, maskContainer || n.getName() == "creditEvent_masked" ? 3 : 2),
              "credit-before-debit mask naming must preserve independent reset identities");
    }
  });
  if (collidingMasks) {
    unsigned debitMasks = 0;
    child.walk([&](NodeOp node) {
      if (node.getName() != "simulationTrigger_creditEvent_masked") return;
      auto gate = node.getInput().getDefiningOp<AndPrimOp>();
      auto negate = gate ? gate.getLhs().getDefiningOp<NotPrimOp>() : NotPrimOp();
      require(gate && negate && gate.getRhs() == debit.getResult() &&
              negate.getInput() == arg(child, 1), "export allocation displaced the debit mask identity");
      ++debitMasks;
    });
    require(debitMasks == 1, "all mask names must be allocated before source export names");
    unsigned exportDrivers = 0;
    for (auto connect : child.getBodyBlock()->getOps<StrictConnectOp>())
      for (unsigned index : {0, 1}) {
        if (connect.getDest() != arg(child, originalPorts + index)) continue;
        auto mask = connect.getSrc().getDefiningOp<NodeOp>();
        require(mask && mask.getName() == (index ? "simulationTrigger_creditEvent_masked" :
                    maskContainer ? "creditEvent_masked_0" : "creditEvent_masked"),
                "renamed export must retain its original masked event driver");
        ++exportDrivers;
      }
    require(exportDrivers == 2, "colliding event exports need one driver each");
  }
  circuit.walk([&](RegOp) { ++registers; });
  for (auto module : circuit.getOps<FModuleOp>()) {
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      ++instances;
      require(instance.getNumResults() == child.getNumPorts() +
                  (ancestorCollision && instance.getModuleName() != "Child") &&
              instance->getAttrOfType<StringAttr>("example.metadata") == "preserve", "source fanout instance replacement");
      unsigned originalDrivers = 0, sinkDrivers = 0;
      for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>()) {
        for (unsigned i = 0; i < 4; ++i) originalDrivers += connect.getDest() == instance.getResult(i);
        if (lastSourceClock) originalDrivers += connect.getDest() == instance.getResult(originalPorts - 1);
        if (childSink) sinkDrivers += connect.getDest() == instance.getResult(originalPorts + exports +
            (ancestorCollision && instance.getModuleName() != "Child"));
      }
      unsigned expectedDrivers = 4 + lastSourceClock;
      if (mode == 15 && module == top && instance.getName() == "second") --expectedDrivers;
      require(originalDrivers == expectedDrivers && sinkDrivers == childSink,
              "source fanout lost or duplicated input connections");
    }
  }
  unsigned localCounters = 0;
  top.walk([&](RegOp reg) {
    bool secondary = reg.getName() == "otherClock_credits" || reg.getName() == "otherClock_debits";
    bool base = reg.getName() == "clock_credits" || reg.getName() == "clock_debits";
    localCounters += secondary || base;
    require(reg.getClockVal() == arg(top, secondary ? 7 : 0),
            "source fanout local counters or base synchronizers use wrong clock");
  });
  bool mixed = mode == 2 || mode == 3 || mode == 6 || mode == 7 || mode >= 9;
  require(localCounters == (mixed ? 4 : 2), "source fanout accounting root name mismatch");
  if (mixedMasks) {
    for (auto node : top.getBodyBlock()->getOps<NodeOp>())
      for (bool secondary : {false, true})
        for (bool isCredit : {true, false}) {
          std::string name = std::string(secondary ? "otherClock_" : "clock_") +
                             (isCredit ? "credits_next" : "debits_next");
          if (node.getName() != name) continue;
          auto add = node.getInput().getDefiningOp<AddPrimOp>();
          require(bool(add), "mixed trigger NEXT must add the event sum");
          SmallVector<Value> leaves, expected;
          std::function<void(Value)> flatten = [&](Value value) {
            if (auto alias = value.getDefiningOp<NodeOp>()) flatten(alias.getInput());
            else if (auto sum = value.getDefiningOp<AddPrimOp>()) {
              flatten(sum.getLhs()); flatten(sum.getRhs());
            } else leaves.push_back(value);
          };
          flatten(add.getRhs());
          for (auto instance : top.getBodyBlock()->getOps<InstanceOp>()) {
            bool primary = instance.getName() == (secondary ? "second" : "first");
            if (primary) expected.push_back(instance.getResult(originalPorts + 1));
            if (primary == isCredit)
              expected.push_back(instance.getResult(originalPorts + (isCredit ? 0 : 2)));
          }
          require(leaves.size() == 2 && expected.size() == 2 &&
                  llvm::all_of(expected, [&](Value value) { return llvm::is_contained(leaves, value); }),
                  "mixed sources must count the shared event once and the independent mask on its own clock");
        }
  }
  if (lastSourceClock && !mixedMasks) {
    // Executed Scala oracle: a shared unmasked target has one export used by
    // both kinds. Credits precede debits for clock selection even when raw
    // annotations put debits first. Distinct targets export once per kind/instance.
    // Reversing annotation clocks swaps the source feeding each domain,
    // without adding another export.
    bool alternateSelected = !collidingMasks && !(mode == 15 || (mode == 14 && relayDepth));
    for (auto node : top.getBodyBlock()->getOps<NodeOp>())
      for (bool secondary : {false, true})
        for (bool credit : {true, false}) {
          std::string name = std::string(secondary ? "otherClock_" : "clock_") +
                             (credit ? "credits_next" : "debits_next");
          if (node.getName() != name || (ancestorCollision && !credit)) continue;
          auto add = node.getInput().getDefiningOp<AddPrimOp>();
          StringRef instanceName = secondary == alternateSelected ? "first" : "second";
          for (auto instance : top.getBodyBlock()->getOps<InstanceOp>())
            if (instance.getName() == instanceName)
              require(add && add.getRhs() == instance.getResult(originalPorts + (sharedTarget ? 0 : !credit)),
                      "last source clock must select the exported event's accounting domain");
        }
  }
  require(instances == 2 + relayDepth && masks == (collidingMasks ? 1 : mixedMasks ? 2 : lastSourceClock ? 0 : duplicateMasked ? 2 : 1) && registers == 9 + 6 * mixed + childSink &&
          circuit->getAttr("rawAnnotations") == b.getArrayAttr({channel}), "source fanout state or cleanup mismatch");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write source fanout candidate"); root->print(out); out << '\n';
  }
}

// Sibling exports and uses in different parent definitions stay separate.
void nestedSiblingSources(MLIRContext &context, unsigned mode, StringRef output, unsigned relayDepth = 1) {
  bool childSink = mode == 1 || mode == 4 || mode == 7;
  std::string childPorts = "in %clock: !firrtl.clock, in %credit: !firrtl.uint<1>, "
    "in %debit: !firrtl.uint<1>, in %reset: !firrtl.uint<1>, out %echo: !firrtl.uint<1>";
  if (childSink) childPorts += ", out %sinkEnabled: !firrtl.uint<1>";
  std::string instancePorts = childPorts;
  instancePorts.erase(std::remove(instancePorts.begin(), instancePorts.end(), '%'), instancePorts.end());
  unsigned originalPorts = childSink ? 6 : 5;
  auto inst = [&](StringRef name, StringRef module) { return "%" + name.str() + ":" + std::to_string(originalPorts) +
    " = firrtl.instance " + name.str() + " @" + module.str() + "(" + instancePorts + ")"; };
  std::string parentPorts = "in %clock: !firrtl.clock, in %credit0: !firrtl.uint<1>, "
    "in %debit0: !firrtl.uint<1>, in %reset0: !firrtl.uint<1>, in %credit1: !firrtl.uint<1>, "
    "in %debit1: !firrtl.uint<1>, in %reset1: !firrtl.uint<1>, in %otherClock: !firrtl.clock, "
    "out %enabled: !firrtl.uint<1>, out %echo0: !firrtl.uint<1>, out %echo1: !firrtl.uint<1>";
  if (childSink) parentPorts += ", out %sink0: !firrtl.uint<1>, out %sink1: !firrtl.uint<1>";
  std::string parentInstancePorts = parentPorts;
  parentInstancePorts.erase(std::remove(parentInstancePorts.begin(), parentInstancePorts.end(), '%'), parentInstancePorts.end());
  unsigned parentPortsBefore = 11 + 2 * childSink;
  auto parentInst = [&](StringRef name, StringRef module) { return "%" + name.str() + ":" +
    std::to_string(parentPortsBefore) + " = firrtl.instance " + name.str() + " @" + module.str() +
    "(" + parentInstancePorts + ")"; };
  std::string relayText = "firrtl.module @Relay(" + parentPorts + ") { " +
    inst("first", "Child") + " " + inst("second", "Child") + " } ";
  if (relayDepth == 2)
    relayText += "firrtl.module @Outer(" + parentPorts + ") { " + parentInst("middle", "Relay") + " } ";
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" attributes {rawAnnotations = []} { "
    "firrtl.module @Child(" + childPorts + ") {} " + relayText + "firrtl.module @Top(" + parentPorts +
    ") { " + parentInst("parent", relayDepth == 2 ? "Outer" : "Relay") + " } } }", &context);
  require(bool(root), "parse source fanout");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto it = circuit.getOps<FModuleOp>().begin(); auto child = *it++;
  SmallVector<FModuleOp> relays;
  for (unsigned depth = 0; depth < relayDepth; ++depth) relays.push_back(*it++);
  auto top = *it;
  OpBuilder b(&context); auto loc = top.getLoc(); auto bit = UIntType::get(&context, 1);
  auto arg = [](FModuleOp m, unsigned i) { return m.getBodyBlock()->getArgument(i); };
  b.setInsertionPointToEnd(child.getBodyBlock());
  auto credit = b.create<NodeOp>(loc, arg(child, 1), b.getStringAttr("creditEvent"));
  b.create<NodeOp>(loc, arg(child, 2), b.getStringAttr("debitEvent"));
  b.create<StrictConnectOp>(loc, arg(child, 4), credit.getResult());
  if (childSink) {
    auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
    auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
    b.create<StrictConnectOp>(loc, arg(child, 5), trigger.getResult());
  }
  for (auto [depth, relay] : llvm::enumerate(relays)) {
    b.setInsertionPointToEnd(relay.getBodyBlock());
    for (auto [index, instance] : llvm::enumerate(relay.getBodyBlock()->getOps<InstanceOp>())) {
      instance->setAttr("example.metadata", b.getStringAttr("preserve"));
      if (depth == 0) {
        bool wrongClock = ((mode == 2 || mode == 9) && index == 1) ||
                          ((mode == 3 || mode == 8) && index == 0);
        b.create<StrictConnectOp>(loc, instance.getResult(0), arg(relay, wrongClock ? 7 : 0));
        for (unsigned i = 1; i <= 3; ++i)
          b.create<StrictConnectOp>(loc, instance.getResult(i), arg(relay, i + 3 * index));
        b.create<StrictConnectOp>(loc, arg(relay, 9 + index), instance.getResult(4));
        if (childSink) b.create<StrictConnectOp>(loc, arg(relay, 11 + index), instance.getResult(5));
      } else {
        for (unsigned i = 0; i < 8; ++i)
          b.create<StrictConnectOp>(loc, instance.getResult(i), arg(relay, i));
        for (unsigned i = 9; i < parentPortsBefore; ++i)
          b.create<StrictConnectOp>(loc, arg(relay, i), instance.getResult(i));
      }
    }
    auto enabled = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
    b.create<StrictConnectOp>(loc, arg(relay, 8), enabled.getResult());
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto parent = *top.getBodyBlock()->getOps<InstanceOp>().begin();
  parent->setAttr("example.metadata", b.getStringAttr("preserve"));
  SmallVector<InstanceOp> topInstances{parent};
  if (mode == 4) {
    auto other = cast<InstanceOp>(b.clone(*parent.getOperation()));
    other.setNameAttr(b.getStringAttr("otherParent"));
    topInstances.push_back(other);
  }
  if (mode >= 5) {
    // The same source definition occurs directly under top and under Relay.
    // These routes have unequal depths and must each contribute one event.
    auto nested = *relays.front().getBodyBlock()->getOps<InstanceOp>().begin();
    auto other = cast<InstanceOp>(b.clone(*nested.getOperation()));
    other.setNameAttr(b.getStringAttr("directChild"));
    for (unsigned i = 0; i < 4; ++i)
      b.create<StrictConnectOp>(loc, other.getResult(i), arg(top, mode == 6 && i == 0 ? 7 : i));
  }
  for (auto instance : topInstances)
    for (unsigned i = 0; i < 8; ++i)
      b.create<StrictConnectOp>(loc, instance.getResult(i), arg(top, i));
  for (unsigned i = 9; i < parentPortsBefore; ++i)
    b.create<StrictConnectOp>(loc, arg(top, i), parent.getResult(i));
  auto one = b.create<ConstantOp>(loc, bit, llvm::APInt(1, 1));
  auto trigger = b.create<NodeOp>(loc, one.getResult(), b.getStringAttr("trigger"));
  b.create<StrictConnectOp>(loc, arg(top, 8), trigger.getResult());
  auto ref = [&](StringRef module, StringRef name) { return b.getStringAttr(("~Top|" + module + ">" + name).str()); };
  SmallVector<Attribute> annotations;
  for (bool credit : {true, false}) {
    NamedAttrList attrs; attrs.set("class", b.getStringAttr(A::InternalTriggerSource));
    attrs.set("target", ref("Child", credit ? "creditEvent" : "debitEvent"));
    attrs.set("clock", ref("Child", "clock")); attrs.set("sourceType", b.getBoolAttr(credit));
    if (credit) attrs.set("reset", ref("Child", "reset"));
    annotations.push_back(attrs.getDictionary(&context));
    // An unmasked duplicate shares every absolute export. Exercise both raw
    // class spellings; consumption still counts annotations, not unique events.
    if (!credit && mode >= 9) {
      annotations.push_back(attrs.getDictionary(&context));
      attrs.set("class", b.getStringAttr(A::TriggerSource));
      annotations.push_back(attrs.getDictionary(&context));
    }
  }
  SmallVector<StringRef> sinkModules{"Top"};
  if (childSink) sinkModules.push_back("Child");
  for (StringRef module : sinkModules)
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref(module, "trigger")), b.getNamedAttr("clock", ref(module, "clock"))}));
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("Top", "clock")}))});
  annotations.push_back(channel); circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(root.get()); unsigned consumed = 99; std::string error;
  auto result = goldengate::wireTriggers(circuit, consumed, error);
  require(succeeded(result), error);
  require(consumed == (mode >= 9 ? 4 : 2) && succeeded(verify(*root)), "invalid source fanout IR");
  require(top.getNumPorts() == 11 + 2 * childSink && child.getNumPorts() == originalPorts + 2 + childSink,
          "source fanout must preserve top IO and share child export definitions");
  require(child.getPortName(originalPorts) == "simulationTrigger_creditEvent_masked" &&
          child.getPortName(originalPorts + 1) == "simulationTrigger_debitEvent", "source fanout export names");
  for (auto [depth, relay] : llvm::enumerate(relays)) {
    require(relay.getNumPorts() == parentPortsBefore + 4 + childSink,
            "sibling parent must relay four separate event exports");
    for (StringRef instance : {"first", "second"})
      for (StringRef event : {"creditEvent_masked", "debitEvent"}) {
        std::string name = "simulationTrigger_" + std::string(depth ? "middle_" : "") +
          instance.str() + "_" + event.str();
        bool found = false;
        for (auto port : relay.getPorts()) found |= port.getName() == name;
        require(found, "sibling relay export name: " + name);
      }
  }
  unsigned instances = 0, masks = 0, registers = 0;
  child.walk([&](NodeOp n) { masks += n.getName() == "creditEvent_masked"; });
  circuit.walk([&](RegOp) { ++registers; });
  for (auto module : circuit.getOps<FModuleOp>()) {
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      ++instances;
      require(instance.getNumResults() == (instance.getModuleName() == "Child" ? child.getNumPorts() : relays.back().getNumPorts()) &&
              instance->getAttrOfType<StringAttr>("example.metadata") == "preserve", "source fanout instance replacement");
      unsigned originalDrivers = 0, sinkDrivers = 0;
      unsigned inputs = instance.getModuleName() == "Child" ? 4 : 8;
      unsigned sinkIndex = instance.getModuleName() == "Child" ? originalPorts + 2 : parentPortsBefore + 4;
      for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>()) {
        for (unsigned i = 0; i < inputs; ++i) originalDrivers += connect.getDest() == instance.getResult(i);
        if (childSink) sinkDrivers += connect.getDest() == instance.getResult(sinkIndex);
      }
      require(originalDrivers == inputs && sinkDrivers == childSink, "source fanout lost or duplicated input connections");
    }
  }
  bool mixed = mode == 2 || mode == 3 || mode == 6 || mode == 8 || mode == 9;
  unsigned localCounters = 0;
  top.walk([&](RegOp reg) {
    bool secondary = reg.getName() == "otherClock_credits" || reg.getName() == "otherClock_debits";
    localCounters += secondary || reg.getName() == "clock_credits" || reg.getName() == "clock_debits";
    require(reg.getClockVal() == arg(top, secondary ? 7 : 0), "sibling source accounting clock root");
  });
  require(localCounters == (mixed ? 4 : 2), "sibling source domain count");
  require(instances == 2 + relayDepth + (mode >= 4) && masks == 1 && registers == 9 + 6 * mixed + childSink &&
          circuit->getAttr("rawAnnotations") == b.getArrayAttr({channel}), "source fanout state or cleanup mismatch");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "cannot write source fanout candidate"); root->print(out); out << '\n';
  }
}

}
// Two independent top-local clocks: Scala groups aliases by root, samples
// each local NEXT into the base clock, then sums full UInt<17> differences.
void clockDomains(MLIRContext &context, StringRef output, bool independentSink = false) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %otherClock: !firrtl.clock,
        in %credit0: !firrtl.uint<1>, in %debit0: !firrtl.uint<1>,
        in %credit1: !firrtl.uint<1>, in %debit1: !firrtl.uint<1>,
        in %reset: !firrtl.uint<1>, out %enabled: !firrtl.uint<1>,
        out %enabledOther: !firrtl.uint<1>) {
        %otherAlias = firrtl.node %otherClock : !firrtl.clock
        %zero = firrtl.constant 0 : !firrtl.uint<1>
        %trigger = firrtl.node %zero : !firrtl.uint<1>
        %triggerOther = firrtl.node %zero : !firrtl.uint<1>
        firrtl.strictconnect %enabled, %trigger : !firrtl.uint<1>
        firrtl.strictconnect %enabledOther, %triggerOther : !firrtl.uint<1>
      }
    }
  })mlir", &context);
  require(bool(root), "two-domain parse");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context);
  auto ref = [&](StringRef name) { return b.getStringAttr(("~Top|Top>" + name).str()); };
  SmallVector<Attribute> annotations;
  for (unsigned i = 0; i < 2; ++i) for (bool credit : {true, false}) {
    NamedAttrList a;
    a.set("class", b.getStringAttr(A::InternalTriggerSource));
    a.set("target", ref(std::string(credit ? "credit" : "debit") + std::to_string(i)));
    a.set("clock", ref(i ? "otherAlias" : "clock"));
    a.set("sourceType", b.getBoolAttr(credit));
    if (credit) a.set("reset", ref("reset"));
    annotations.push_back(a.getDictionary(&context));
  }
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
    b.getNamedAttr("target", ref("trigger")), b.getNamedAttr("clock", ref("clock"))}));
  if (independentSink)
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref("triggerOther")), b.getNamedAttr("clock", ref("otherAlias"))}));
  auto channel = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
    b.getNamedAttr("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel))})),
    b.getNamedAttr("sinks", b.getArrayAttr({ref("clock")}))});
  annotations.push_back(channel);
  auto incomplete = annotations; incomplete.erase(incomplete.begin() + 3);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(incomplete));
  auto before = dump(root.get()); unsigned consumed; std::string error;
  require(failed(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 0 &&
          dump(root.get()) == before && error.find("each source clock domain") != std::string::npos,
          "incomplete clock domain must fail atomically");
  if (independentSink) {
    auto invalidSink = annotations;
    invalidSink[5] = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::InternalTriggerSink)),
      b.getNamedAttr("target", ref("triggerOther")), b.getNamedAttr("clock", ref("reset"))});
    circuit->setAttr("rawAnnotations", b.getArrayAttr(invalidSink));
    before = dump(root.get());
    require(failed(goldengate::wireTriggers(circuit, consumed, error)) && consumed == 0 &&
            dump(root.get()) == before && error.find("input Clock alias") != std::string::npos,
            "invalid second sink clock must fail atomically");
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  require(succeeded(goldengate::wireTriggers(circuit, consumed, error)), error);
  require(consumed == 4 && succeeded(verify(*root)) && top.getNumPorts() == 9 &&
          circuit->getAttr("rawAnnotations") == b.getArrayAttr({channel}), "two-domain structure/cleanup");
  std::map<std::string, Value> values; SmallVector<RegOp> regs;
  llvm::DenseMap<Value, Value> drivers;
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) values[name.getValue().str()] = op->getResult(0);
    if (auto reg = dyn_cast<RegOp>(op)) regs.push_back(reg);
    if (auto connect = dyn_cast<StrictConnectOp>(op)) drivers[connect.getDest()] = connect.getSrc();
  });
  require(regs.size() == (independentSink ? 16 : 15),
          "two domains need twelve local/sample plus two global and one register per sink");
  for (auto reg : regs) {
    bool secondary = reg.getName() == "otherClock_credits" || reg.getName() == "otherClock_debits";
    Value expected = reg.getName() == "trigger_sync_0" ? values.at("otherAlias") :
                     top.getBodyBlock()->getArgument(secondary ? 1 : 0);
    require(reg.getClockVal() == expected, "domain register clock");
  }
  require(drivers.lookup(values.at("trigger_sync")) == values.at("trigger_source"),
          "base sink samples shared trigger enable");
  if (independentSink)
    require(drivers.lookup(values.at("trigger_sync_0")) == values.at("trigger_source") &&
            cast<NodeOp>(values.at("triggerOther").getDefiningOp()).getInput() == values.at("trigger_sync_0"),
            "second sink samples the same enable through one local register");
  // Evaluate the actual generated SSA cone with independent clock edges.
  llvm::DenseMap<Value, uint64_t> state, inputs;
  std::function<uint64_t(Value)> eval = [&](Value v) -> uint64_t {
    if (state.count(v)) return state.lookup(v);
    if (inputs.count(v)) return inputs.lookup(v);
    uint64_t n;
    if (auto node = v.getDefiningOp<NodeOp>()) n = eval(node.getInput());
    else if (auto c = v.getDefiningOp<ConstantOp>()) n = c.getValueAttr().getValue().getZExtValue();
    else if (auto op = v.getDefiningOp<NotPrimOp>()) n = ~eval(op.getInput());
    else if (auto op = v.getDefiningOp<AndPrimOp>()) n = eval(op.getLhs()) & eval(op.getRhs());
    else if (auto op = v.getDefiningOp<AddPrimOp>()) n = eval(op.getLhs()) + eval(op.getRhs());
    else if (auto op = v.getDefiningOp<SubPrimOp>()) n = eval(op.getLhs()) - eval(op.getRhs());
    else if (auto op = v.getDefiningOp<NEQPrimOp>()) n = eval(op.getLhs()) != eval(op.getRhs());
    else if (auto op = v.getDefiningOp<BitsPrimOp>()) n = eval(op.getInput()) >> op.getLo();
    else throw std::runtime_error("unsupported two-domain evaluation");
    return n & ((uint64_t(1) << *cast<UIntType>(v.getType()).getWidth()) - 1);
  };
  for (auto reg : regs) state[reg.getResult()] = 0;
  for (unsigned cycle = 0; cycle < 4096; ++cycle) {
    if (cycle % 128 == 0) for (auto reg : regs)
      state[reg.getResult()] = (uint64_t(1) << *cast<UIntType>(reg.getResult().getType()).getWidth()) - 1;
    for (unsigned i = 2; i <= 6; ++i) inputs[top.getBodyBlock()->getArgument(i)] = (cycle >> (i - 2)) & 1;
    uint64_t creditDiff = 0, debitDiff = 0;
    for (StringRef name : {"clock", "otherClock"}) for (bool credit : {true, false}) {
      auto stem = name.str() + (credit ? "_credits" : "_debits");
      uint64_t diff = (state.lookup(values.at(stem + "_next_count_sync_s1")) -
                       state.lookup(values.at(stem + "_next_count_sync_s2"))) & 0x1ffff;
      (credit ? creditDiff : debitDiff) += diff;
    }
    auto cn = state.lookup(values.at("totalCredits")) + creditDiff;
    auto dn = state.lookup(values.at("totalDebits")) + debitDiff;
    require(eval(values.at("totalCredits_next")) == cn && eval(values.at("totalDebits_next")) == dn &&
            eval(values.at("trigger_source")) == unsigned(cn != dn), "two-domain full NEXT aggregation");
    auto next = state;
    for (auto reg : regs) {
      bool secondary = reg.getClockVal() == top.getBodyBlock()->getArgument(1) ||
                       reg.getClockVal() == values.at("otherAlias");
      if (secondary ? cycle % 3 != 0 : cycle % 2 == 0) next[reg.getResult()] = eval(drivers.lookup(reg.getResult()));
    }
    if (independentSink)
      require(next.lookup(values.at("trigger_sync_0")) ==
              (cycle % 3 != 0 ? unsigned(cn != dn) : state.lookup(values.at("trigger_sync_0"))),
              "second sink updates only on its local clock edge");
    state = std::move(next);
  }
  if (!output.empty()) { std::error_code ec; llvm::raw_fd_ostream out(output, ec);
    require(!ec, "two-domain output"); root->print(out); out << '\n'; }
}

int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context, true, true, argc > 1 ? argv[1] : "");
    run(context, false, false, "");
    clockDomains(context, argc > 45 ? argv[45] : "");
    clockDomains(context, argc > 46 ? argv[46] : "", true);
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
    childSources(context, 0, argc > 21 ? argv[21] : "", 1, true);
    childSources(context, 1, argc > 22 ? argv[22] : "", 1, true);
    childSources(context, 6, argc > 23 ? argv[23] : "", 1, true);
    childSources(context, 0, "", 0, true);
    childSources(context, 0, "", 2, true);
    for (unsigned mode : {2, 3, 4, 5, 7, 9, 10}) childSources(context, mode, "", 1, true);
    sharedSinks(context, 0, argc > 24 ? argv[24] : "");
    sharedSinks(context, 1, argc > 25 ? argv[25] : "");
    sharedSinks(context, 2, argc > 26 ? argv[26] : "");
    sharedSinks(context, 3, "");
    fanoutSinks(context, 0, argc > 27 ? argv[27] : "");
    fanoutSinks(context, 1, argc > 28 ? argv[28] : "");
    fanoutSinks(context, 2, argc > 47 ? argv[47] : "");
    fanoutSinks(context, 3, argc > 29 ? argv[29] : "");
    fanoutSinks(context, 6, argc > 30 ? argv[30] : "");
    fanoutSinks(context, 7, "");
    fanoutSinks(context, 8, "");
    fanoutSinks(context, 4, argc > 31 ? argv[31] : "");
    fanoutSinks(context, 9, argc > 32 ? argv[32] : "");
    fanoutSinks(context, 10, argc > 48 ? argv[48] : "");
    fanoutSinks(context, 12, argc > 49 ? argv[49] : "");
    for (unsigned mode : {11, 13, 14, 15}) fanoutSinks(context, mode, "");
    fanoutSinks(context, 5, "");
    fanoutSources(context, 0, argc > 33 ? argv[33] : "");
    fanoutSources(context, 1, argc > 34 ? argv[34] : "");
    fanoutSources(context, 2, "");
    fanoutSources(context, 3, "");
    fanoutSources(context, 0, argc > 35 ? argv[35] : "", 1);
    fanoutSources(context, 1, argc > 36 ? argv[36] : "", 1);
    fanoutSources(context, 1, argc > 37 ? argv[37] : "", 2);
    for (unsigned depth : {1, 2})
      for (unsigned mode : {2, 3}) fanoutSources(context, mode, "", depth);
    fanoutSources(context, 4, argc > 50 ? argv[50] : "");
    fanoutSources(context, 5, argc > 51 ? argv[51] : "");
    fanoutSources(context, 5, argc > 52 ? argv[52] : "", 1);
    fanoutSources(context, 5, argc > 53 ? argv[53] : "", 2);
    fanoutSources(context, 2, argc > 54 ? argv[54] : "");
    fanoutSources(context, 6, argc > 55 ? argv[55] : "");
    fanoutSources(context, 6, argc > 56 ? argv[56] : "", 1);
    fanoutSources(context, 7, argc > 57 ? argv[57] : "", 2);
    for (unsigned depth : {0, 1, 2}) fanoutSources(context, 8, "", depth);
    fanoutSources(context, 9, argc > 58 ? argv[58] : "");
    fanoutSources(context, 10, argc > 59 ? argv[59] : "");
    fanoutSources(context, 10, argc > 60 ? argv[60] : "", 1);
    fanoutSources(context, 10, argc > 61 ? argv[61] : "", 2);
    fanoutSources(context, 11, argc > 62 ? argv[62] : "");
    fanoutSources(context, 12, argc > 63 ? argv[63] : "");
    fanoutSources(context, 12, argc > 64 ? argv[64] : "", 1);
    fanoutSources(context, 12, argc > 65 ? argv[65] : "", 2);
    fanoutSources(context, 13, argc > 66 ? argv[66] : "");
    fanoutSources(context, 14, argc > 67 ? argv[67] : "");
    fanoutSources(context, 14, argc > 68 ? argv[68] : "", 1);
    fanoutSources(context, 14, argc > 69 ? argv[69] : "", 2);
    fanoutSources(context, 13, argc > 76 ? argv[76] : "", 0, true, true);
    fanoutSources(context, 14, argc > 77 ? argv[77] : "", 0, true, true);
    fanoutSources(context, 14, argc > 78 ? argv[78] : "", 1, true, true);
    fanoutSources(context, 14, argc > 79 ? argv[79] : "", 2, true, true);
    fanoutSources(context, 13, argc > 80 ? argv[80] : "", 0, false, false, true);
    fanoutSources(context, 14, argc > 81 ? argv[81] : "", 0, false, false, true);
    fanoutSources(context, 14, argc > 82 ? argv[82] : "", 1, false, false, true);
    fanoutSources(context, 14, argc > 83 ? argv[83] : "", 2, false, false, true);
    // Ancestor masks may occupy descendant export names. A local source
    // matching a flattened descendant identity is rejected by Scala TopWiring.
    fanoutSources(context, 14, argc > 84 ? argv[84] : "", 1, false, false, true, 1);
    fanoutSources(context, 14, argc > 85 ? argv[85] : "", 2, false, false, true, 1);
    fanoutSources(context, 14, "", 1, false, false, true, 2);
    fanoutSources(context, 14, "", 2, false, false, true, 2);
    // Scala cannot reconstruct renamed top exports, including masks allocated
    // before TopWiring. Reject these namespaces without modifying any IR.
    fanoutSources(context, 14, "", 0, false, false, true, 0, 1);
    fanoutSources(context, 14, "", 1, false, false, true, 0, 2);
    fanoutSources(context, 14, "", 2, false, false, true, 0, 3);
    fanoutSources(context, 14, "", 1, false, false, true, 0, 4);
    fanoutSources(context, 14, argc > 86 ? argv[86] : "", 1, false, false, true, 0, 5);
    // SFC normalizes aggregates before TriggerWiring: leaf names collide,
    // while an aggregate container with distinct leaves does not.
    for (unsigned collision : {6u, 7u, 8u, 9u})
      fanoutSources(context, 14, "", 1, false, false, true, 0, collision);
    fanoutSources(context, 14, argc > 87 ? argv[87] : "", 1, false, false, true, 0, 10);
    // Scala frees disappearing aggregate containers before allocating masks.
    // CIRCT keeps the container, so its node name and export identity differ.
    fanoutSources(context, 14, argc > 88 ? argv[88] : "", 1, false, false, true, 0, 0, true);
    fanoutSources(context, 14, "", 1, false, false, true, 0, 3, true);
    for (unsigned mode : {12u, 13u, 14u})
      eventTargets(context, mode, argc > mode + 77 ? argv[mode + 77] : "");
    // Existing aggregate leaves reserve descendant/relay export names after
    // Scala normalization. Port renaming must preserve the event's SSA driver.
    // Disappearing containers, in contrast, do not occupy their flattened
    // namespace. Exports must retain the SFC spelling through LowerTypes.
    for (unsigned leaf : {1u, 2u, 3u, 4u, 5u, 6u})
      fanoutSources(context, 14, argc > leaf + 91 ? argv[leaf + 91] : "",
                    1, false, false, true, 0, 0, false, leaf);
    run(context, true, false, argc > 74 ? argv[74] : "", true);
    run(context, false, false, argc > 75 ? argv[75] : "", true);
    fanoutSources(context, 13, argc > 70 ? argv[70] : "", 0, true);
    fanoutSources(context, 14, argc > 71 ? argv[71] : "", 0, true);
    fanoutSources(context, 14, argc > 72 ? argv[72] : "", 1, true);
    fanoutSources(context, 14, argc > 73 ? argv[73] : "", 2, true);
    for (unsigned depth : {0, 1, 2})
      for (unsigned mode : {15, 16}) fanoutSources(context, mode, "", depth);
    nestedSiblingSources(context, 0, argc > 38 ? argv[38] : "");
    nestedSiblingSources(context, 1, argc > 39 ? argv[39] : "");
    nestedSiblingSources(context, 1, argc > 40 ? argv[40] : "", 2);
    nestedSiblingSources(context, 4, argc > 41 ? argv[41] : "");
    nestedSiblingSources(context, 5, argc > 42 ? argv[42] : "");
    nestedSiblingSources(context, 7, argc > 43 ? argv[43] : "");
    nestedSiblingSources(context, 7, argc > 44 ? argv[44] : "", 2);
    for (unsigned depth : {1, 2})
      for (unsigned mode : {2, 3, 6, 8, 9}) nestedSiblingSources(context, mode, "", depth);
    llvm::outs() << "TriggerWiring local accounting and atomic preflight PASS\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
