// See LICENSE for license details.
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEFiredRegister.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/FAMEFinishing.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include <map>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context, bool clockToken = false) {
  std::string text = R"mlir(
    module { firrtl.circuit "Model" {
      firrtl.module @Model(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, in %enable: !firrtl.uint<1>,
        in %done: !firrtl.uint<1>,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        in %virtualInput_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %virtualOutput_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {
        %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
        firrtl.strictconnect %targetCycleFinishing, %done : !firrtl.uint<1>
      }
    } }
  )mlir";
  if (clockToken) {
    auto pos = text.find(") {");
    text.insert(pos,
                ", in %tick_sink: !firrtl.bundle<ready flip: uint<1>, valid: "
                "uint<1>, bits: clock>");
  }
  return parseSourceString<ModuleOp>(text, &context);
}
FModuleOp model(ModuleOp root) {
  auto circuit = *root.getOps<CircuitOp>().begin();
  return *circuit.getOps<FModuleOp>().begin();
}
SmallVector<goldengate::FAMEFiredChannel> channels(FModuleOp m) {
  OpBuilder b(&m.getBodyBlock()->front());
  auto one = b.create<ConstantOp>(
      m.getLoc(), UIntType::get(m.getContext(), 1), APInt(1, 1));
  return {{"input", true, m.getBodyBlock()->getArgument(2)},
          {"output", false, m.getBodyBlock()->getArgument(2)},
          {"virtualInput", true, one, false},
          {"virtualOutput", false, one, false}};
}
unsigned eval(Value v, const llvm::DenseMap<Value, unsigned> &values) {
  if (auto it = values.find(v); it != values.end())
    return it->second;
  if (auto c = v.getDefiningOp<ConstantOp>())
    return c.getValue().getZExtValue();
  if (auto w = v.getDefiningOp<WireOp>()) {
    for (auto *user : w.getResult().getUsers())
      if (auto c = dyn_cast<StrictConnectOp>(user); c && c.getDest() == v)
        return eval(c.getSrc(), values);
  }
  if (auto p = v.getDefiningOp<NotPrimOp>())
    return !eval(p.getInput(), values);
  if (auto p = v.getDefiningOp<AndPrimOp>())
    return eval(p.getLhs(), values) & eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<OrPrimOp>())
    return eval(p.getLhs(), values) | eval(p.getRhs(), values);
  if (auto p = v.getDefiningOp<MuxPrimOp>())
    return eval(p.getSel(), values) ? eval(p.getHigh(), values)
                                    : eval(p.getLow(), values);
  if (auto p = v.getDefiningOp<AsUIntPrimOp>())
    return eval(p.getInput(), values);
  if (auto p = v.getDefiningOp<ConstCastOp>())
    return eval(p.getInput(), values);
  throw std::runtime_error("unexpected transition operation");
}
void behavior(MLIRContext &context) {
  auto root = fixture(context);
  require(bool(root), "fixture parse failed");
  auto m = model(*root);
  auto cs = channels(m);
  std::string error;
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  std::string created = dump(*root);
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  require(created == dump(*root), "register creation is not idempotent");
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)),
          error.c_str());
  require(succeeded(verify(*root)), "rewritten IR does not verify");
  unsigned states = 0;
  for (const auto &ch : cs) {
    RegResetOp reg;
    for (auto r : m.getOps<RegResetOp>())
      if (r.getName() == ch.name + "_fired_0")
        reg = r;
    unsigned reset = unsigned(ch.isInput && ch.hasClockDomain);
    require(reg && reg.getClockVal() == m.getBodyBlock()->getArgument(0) &&
                reg.getResetSignal() == m.getBodyBlock()->getArgument(1),
            "wrong host controls");
    require(eval(reg.getResetValue(), llvm::DenseMap<Value, unsigned>()) ==
                reset,
            "wrong reset value");
    StrictConnectOp transition;
    for (auto c : m.getOps<StrictConnectOp>())
      if (c.getDest() == reg.getResult())
        transition = c;
    require(bool(transition), "missing fired transition");
    // SFC genMetadata/updateFiredReg: reset has priority over completion,
    // completion has priority over a coincident ready/valid handshake.
    for (unsigned flags = 0; flags < 64; ++flags) {
      unsigned old = flags & 1, ready = (flags >> 1) & 1,
               valid = (flags >> 2) & 1;
      unsigned done = (flags >> 3) & 1, enable = (flags >> 4) & 1,
               hostReset = (flags >> 5) & 1;
      llvm::DenseMap<Value, unsigned> values{
          {reg.getResult(), old},
          {m.getBodyBlock()->getArgument(2), enable},
          {m.getBodyBlock()->getArgument(3), done}};
      for (auto f : m.getOps<SubfieldOp>())
        if (f.getInput() ==
            m.getBodyBlock()->getArgument(ch.name == "input"          ? 4
                                          : ch.name == "output"       ? 5
                                          : ch.name == "virtualInput" ? 6
                                                                      : 7))
          values[f.getResult()] = f.getFieldName() == "ready" ? ready : valid;
      unsigned result = hostReset ? eval(reg.getResetValue(), values)
                                  : eval(transition.getSrc(), values);
      unsigned expected = hostReset ? reset
                          : done    ? !(ch.hasClockDomain ? enable : 1)
                                    : old | (ready & valid);
      require(result == expected,
              "reset/finish/handshake priority differs from SFC");
      ++states;
    }
  }
  llvm::outs() << "Passed " << states
               << " clocked/virtual input/output transition and reset cases\n";
}
void rejections(MLIRContext &context) {
  for (unsigned bad = 0; bad < 7; ++bad) {
    auto root = fixture(context);
    require(bool(root), "fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    RegResetOp last;
    for (auto reg : m.getOps<RegResetOp>())
      if (reg.getName() == "virtualOutput_fired_0")
        last = reg;
    OpBuilder b(last);
    if (bad == 0 || bad == 1) {
      RegResetOp reg = last;
      if (bad == 1)
        for (auto r : m.getOps<RegResetOp>())
          if (r.getName() == "input_fired_0")
            reg = r;
      reg.getResetValueMutable().assign(b.create<ConstantOp>(
          m.getLoc(), UIntType::get(&context, 1), APInt(1, bad == 0)));
    }
    if (bad == 2)
      last.getResetValueMutable().assign(m.getBodyBlock()->getArgument(2));
    if (bad == 3)
      last.getClockValMutable().assign(b.create<AsClockPrimOp>(
          m.getLoc(), m.getBodyBlock()->getArgument(1)));
    if (bad == 4)
      last.getResetSignalMutable().assign(m.getBodyBlock()->getArgument(2));
    if (bad == 5)
      cs.back().clockDomainEnable = m.getBodyBlock()->getArgument(2);
    if (bad == 6)
      cs.back().clockDomainEnable = b.create<ConstantOp>(
          m.getLoc(), UIntType::get(&context, 1), APInt(1, 0));
    auto before = dump(*root);
    require(failed(goldengate::rewriteFAMEFiredStates(m, cs, error)) &&
                !error.empty(),
            "invalid contract accepted");
    require(before == dump(*root), "failed rewrite partially mutated channels");
    if (bad < 5) {
      require(failed(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
              "invalid existing reset accepted");
      require(before == dump(*root), "failed register creation mutated IR");
    }
  }
  llvm::outs()
      << "Passed seven atomic reset/host-control/virtual-enable rejections\n";
}

void collidingIdentities(MLIRContext &context) {
  auto root = fixture(context);
  auto m = model(*root);
  auto cs = channels(m);
  OpBuilder b(&m.getBodyBlock()->front());
  auto bit = UIntType::get(&context, 1);
  Value one = b.create<ConstantOp>(m.getLoc(), bit, APInt(1, 1));
  auto target = b.create<RegResetOp>(m.getLoc(), bit,
      m.getBodyBlock()->getArgument(0), m.getBodyBlock()->getArgument(1),
      one, "input_fired_0");
  // This register even has the expected host controls/reset, but is target
  // state. Nodes and wires must also participate in the namespace.
  b.create<NodeOp>(m.getLoc(), one, "output_fired");
  b.create<WireOp>(m.getLoc(), bit, "output_fired_0");
  b.create<WireOp>(m.getLoc(), bit, "virtualInput_fired_0");
  b.setInsertionPointToEnd(m.getBodyBlock());
  b.create<StrictConnectOp>(m.getLoc(), target.getResult(), one);
  std::string error;
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  goldengate::FAMEFiredRegisterIndex index;
  require(succeeded(index.collect(m, error)), error.c_str());
  for (const auto &[channel, expected] :
       std::map<std::string, std::string>{{"input", "input_fired_1"},
          {"output", "output_fired_1_0"},
          {"virtualInput", "virtualInput_fired_1"},
          {"virtualOutput", "virtualOutput_fired_0"}}) {
    auto reg = index.lookup(channel).getDefiningOp<RegResetOp>();
    require(reg && reg.getName() == expected, "SFC namespace allocation differs");
    llvm::outs() << "IDENTITY " << channel << " " << reg.getName() << "\n";
  }
  // Identity persists through textual IR boundaries and subsequent renaming.
  index.lookup("output").getDefiningOp<RegResetOp>().setName("renamed_host_state");
  root = parseSourceString<ModuleOp>(dump(*root), &context);
  require(bool(root), "identity serialization failed");
  m = model(*root);
  cs = channels(m);
  auto before = dump(*root);
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)) &&
              before == dump(*root), "renamed host state was recreated");
  goldengate::FAMEFiredRegisterIndex renamed;
  require(succeeded(renamed.collect(m, error)), error.c_str());
  SmallVector<goldengate::LocalChannelDependency> deps{
      {"output", {"input"}, {}, {}},
      {"virtualOutput", {"virtualInput"}, {}, {}}};
  require(succeeded(goldengate::rewriteFAMEOutputValids(m, deps, error)), error.c_str());
  require(succeeded(goldengate::rewriteFAMEFinishing(
              m, {"input", "virtualInput"}, {"output", "virtualOutput"}, "", error)),
          error.c_str());
  require(succeeded(goldengate::rewriteFAMEInputReadies(
              m, {"input", "virtualInput"}, error)), error.c_str());
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)), error.c_str());
  auto drive = [&](Value dest) -> Value {
    for (auto c : m.getOps<StrictConnectOp>())
      if (c.getDest() == dest)
        return c.getSrc();
    return {};
  };
  Value finishing;
  for (auto w : m.getOps<WireOp>())
    if (w.getName() == "targetCycleFinishing")
      finishing = w.getResult();
  // Exhaust all two-input/two-output token and fired-state combinations.
  for (unsigned flags = 0; flags < 256; ++flags) {
    unsigned iv = flags & 1, vv = (flags >> 1) & 1,
             ready = (flags >> 2) & 1, vr = (flags >> 3) & 1,
             inFired = (flags >> 4) & 1, viFired = (flags >> 5) & 1,
             outFired = (flags >> 6) & 1, voFired = (flags >> 7) & 1;
    unsigned ov = iv & !outFired, vov = vv & !voFired;
    unsigned done = iv & vv & (outFired | ready) & (voFired | vr);
    llvm::DenseMap<Value, unsigned> values{
        {renamed.lookup("input"), inFired},
        {renamed.lookup("virtualInput"), viFired},
        {renamed.lookup("output"), outFired},
        {renamed.lookup("virtualOutput"), voFired},
        {m.getBodyBlock()->getArgument(2), 1}};
    for (auto f : m.getOps<SubfieldOp>()) {
      auto arg = dyn_cast<BlockArgument>(f.getInput());
      require(bool(arg), "unexpected control subfield");
      unsigned p = arg.getArgNumber();
      if (f.getFieldName() == "valid")
        values[f.getResult()] = p == 4 ? iv : p == 6 ? vv : p == 5 ? ov : vov;
      if (f.getFieldName() == "ready")
        values[f.getResult()] = p == 5 ? ready : p == 7 ? vr
                                   : p == 4 ? done & !inFired : done & !viFired;
    }
    require(eval(drive(finishing), values) == done, "identity finishing differs");
    for (auto c : m.getOps<StrictConnectOp>()) {
      auto f = c.getDest().getDefiningOp<SubfieldOp>();
      if (!f)
        continue;
      unsigned p = cast<BlockArgument>(f.getInput()).getArgNumber();
      unsigned expected = p == 4 ? done & !inFired : p == 6 ? done & !viFired
                            : p == 5 ? ov : vov;
      require(eval(c.getSrc(), values) == expected, "identity handshake differs");
      values[c.getDest()] = expected;
      // All equivalent subfield SSA values denote the same port field.
      for (auto other : m.getOps<SubfieldOp>())
        if (other.getInput() == f.getInput() &&
            other.getFieldIndex() == f.getFieldIndex())
          values[other.getResult()] = expected;
    }
    for (const auto &ch : cs) {
      Value fired = renamed.lookup(ch.name);
      unsigned old = values.lookup(fired);
      unsigned firing = ch.name == "input" ? iv & done & !inFired
                       : ch.name == "virtualInput" ? vv & done & !viFired
                       : ch.name == "output" ? ready & ov : vr & vov;
      require(eval(drive(fired), values) == (done ? 0 : old | firing),
              "identity next state differs");
    }
  }
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName() == "input_fired_0")
      require(drive(reg.getResult()) == reg.getResetValue() &&
                  !reg->hasAttr(goldengate::fameFiredChannelAttr),
              "target register was consumed as fired state");
  require(succeeded(verify(*root)), "collision controls failed verification");
  // A forged duplicate must reject every consumer before any IR mutation.
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName() == "input_fired_0")
      reg->setAttr(goldengate::fameFiredChannelAttr, b.getStringAttr("input"));
  before = dump(*root);
  require(failed(goldengate::ensureFAMEFiredRegisters(m, cs, error)) &&
          failed(goldengate::rewriteFAMEOutputValids(m, deps, error)) &&
          failed(goldengate::rewriteFAMEInputReadies(m, {"input"}, error)) &&
          failed(goldengate::rewriteFAMEFiredStates(m, cs, error)) &&
          failed(goldengate::rewriteFAMEFinishing(m, {"input", "virtualInput"},
                    {"output", "virtualOutput"}, "", error)) &&
          before == dump(*root), "duplicate identity was not rejected atomically");
  llvm::outs() << "Passed 256 collision/rename control combinations and atomic duplicate rejections\n";
}
// FAMETransform topRules: VirtualClockChannel.isValid = 1 and
// VirtualClockChannel.setReady = EmptyStmt. Exercise the same data predicate
// with both a real clock token and the virtual channel.
void finishingBehavior(MLIRContext &context) {
  unsigned cases = 0;
  for (bool clockToken : {false, true}) {
    auto root = fixture(context, clockToken);
    require(bool(root), "finishing fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    unsigned ports = m.getNumPorts();
    require(
        succeeded(goldengate::rewriteFAMEFinishing(m,
                                                   {"input", "virtualInput"},
                                                   {"output", "virtualOutput"},
                                                   clockToken ? "tick" : "",
                                                   error)),
        error.c_str());
    require(succeeded(verify(*root)), "finishing IR failed verification");
    require(m.getNumPorts() == ports, "virtual clock created a model port");
    Value next, clockReady;
    for (auto c : m.getOps<StrictConnectOp>()) {
      if (auto w = c.getDest().getDefiningOp<WireOp>();
          w && w.getName() == "targetCycleFinishing")
        next = c.getSrc();
      if (auto f = c.getDest().getDefiningOp<SubfieldOp>();
          clockToken && f && f.getInput() == m.getBodyBlock()->getArgument(8) &&
          f.getFieldName() == "ready")
        clockReady = c.getSrc();
    }
    require(bool(next) && bool(clockReady) == clockToken,
            "wrong finishing/clock ready controls");
    for (unsigned flags = 0; flags < (clockToken ? 512u : 256u); ++flags) {
      unsigned inputValid = flags & 1, virtualValid = (flags >> 1) & 1;
      unsigned outFired = (flags >> 2) & 1, outReady = (flags >> 3) & 1,
               outValid = (flags >> 4) & 1;
      unsigned virtualFired = (flags >> 5) & 1, virtualReady = (flags >> 6) & 1,
               virtualOutValid = (flags >> 7) & 1;
      unsigned tickValid = clockToken ? (flags >> 8) & 1 : 1;
      llvm::DenseMap<Value, unsigned> values;
      for (auto r : m.getOps<RegResetOp>()) {
        if (r.getName() == "output_fired_0")
          values[r.getResult()] = outFired;
        if (r.getName() == "virtualOutput_fired_0")
          values[r.getResult()] = virtualFired;
      }
      for (auto f : m.getOps<SubfieldOp>()) {
        auto port = dyn_cast<BlockArgument>(f.getInput());
        if (!port)
          continue;
        unsigned i = port.getArgNumber();
        if (i == 4 && f.getFieldName() == "valid")
          values[f.getResult()] = inputValid;
        if (i == 6 && f.getFieldName() == "valid")
          values[f.getResult()] = virtualValid;
        if (i == 5)
          values[f.getResult()] =
              f.getFieldName() == "ready" ? outReady : outValid;
        if (i == 7)
          values[f.getResult()] =
              f.getFieldName() == "ready" ? virtualReady : virtualOutValid;
        if (i == 8 && f.getFieldName() == "valid")
          values[f.getResult()] = tickValid;
      }
      unsigned dataReady = inputValid & virtualValid &
                           (outFired | (outReady & outValid)) &
                           (virtualFired | (virtualReady & virtualOutValid));
      require(eval(next, values) == (dataReady & tickValid),
              "finishing differs from SFC topRules");
      if (clockToken)
        require(eval(clockReady, values) == dataReady,
                "clock ready must not depend on clock valid");
      ++cases;
    }
  }
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(context, bad == 3 || bad == 5);
    require(bool(root), "rejection fixture parse failed");
    auto m = model(*root);
    auto cs = channels(m);
    if (bad == 5)
      cs.push_back({"tick", true, m.getBodyBlock()->getArgument(2)});
    std::string error;
    require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
            error.c_str());
    SmallVector<std::string> inputs{"input", "virtualInput"},
        outputs{"output", "virtualOutput"};
    if (bad == 0)
      inputs.pop_back();
    if (bad == 1)
      inputs.push_back("input");
    if (bad == 4)
      outputs.pop_back();
    if (bad == 5)
      inputs.push_back("tick");
    auto before = dump(*root);
    require(failed(goldengate::rewriteFAMEFinishing(
                m, inputs, outputs, bad == 2 ? "missing" : "", error)) &&
                !error.empty(),
            "invalid finishing channel coverage accepted");
    require(before == dump(*root), "failed finishing rewrite mutated IR");
  }
  llvm::outs() << "Passed " << cases
               << " clocked/virtual completion cases and six atomic coverage "
                  "rejections\n";
}
void noDataFinishingRejections(MLIRContext &context) {
  for (bool explicitClock : {false, true}) {
    std::string text = R"mlir(
      module { firrtl.circuit "Model" {
        firrtl.module @Model(in %hostClock: !firrtl.clock,
          in %hostReset: !firrtl.uint<1>) {
          %targetCycleFinishing = firrtl.wire : !firrtl.uint<1>
          firrtl.strictconnect %targetCycleFinishing, %hostReset : !firrtl.uint<1>
        }
      } }
    )mlir";
    if (explicitClock)
      text.insert(text.find(") {"),
                  ", in %tick_sink: !firrtl.bundle<ready flip: uint<1>, "
                  "valid: uint<1>, bits: clock>");
    auto root = parseSourceString<ModuleOp>(text, &context);
    require(bool(root), "no-data model fixture parse failed");
    auto before = dump(*root);
    std::string error;
    require(failed(goldengate::rewriteFAMEFinishing(
                model(*root), {}, {}, explicitClock ? "tick" : "", error)) &&
                error == "FAME model has no data channels",
            "empty completion reduction did not reject like Scala And.reduce");
    require(before == dump(*root), "no-data rejection mutated FIRRTL IR");
  }
  llvm::outs() << "Passed two atomic no-data completion rejections\n";
}
// Emit named boolean operations for a structured comparison with optimized
// SFC RTL. This preserves operands and channel identity while ignoring SSA IDs.
std::string predicate(FModuleOp m, Value value) {
  if (auto f = value.getDefiningOp<SubfieldOp>()) {
    if (auto port = dyn_cast<BlockArgument>(f.getInput()))
      return m.getPortName(port.getArgNumber()).str() + "_" +
             f.getFieldName().str();
  }
  if (auto r = value.getDefiningOp<RegResetOp>())
    return r.getName().str();
  if (auto c = value.getDefiningOp<ConstantOp>())
    return std::to_string(c.getValue().getZExtValue());
  if (auto a = value.getDefiningOp<AndPrimOp>())
    return "(" + predicate(m, a.getLhs()) + " & " + predicate(m, a.getRhs()) +
           ")";
  if (auto o = value.getDefiningOp<OrPrimOp>())
    return "(" + predicate(m, o.getLhs()) + " | " + predicate(m, o.getRhs()) +
           ")";
  if (auto n = value.getDefiningOp<NotPrimOp>())
    return "(~" + predicate(m, n.getInput()) + ")";
  throw std::runtime_error("unexpected completion operation");
}
// Reapply the production output-valid pass using the independently captured
// pre-channelization dependency analysis, then expose its Boolean trees for
// comparison with immutable SFC RTL. Do not recover inputs from old rules.
void outputValidsBoundary(MLIRContext &context, const char *path,
                         const char *dependencyPath) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "output-valid boundary parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp m;
  for (auto candidate : circuit.getOps<FModuleOp>())
    if (candidate.getName() == "FireSim") m = candidate;
  require(bool(m), "output-valid FireSim model missing");
  auto buffer = llvm::MemoryBuffer::getFile(dependencyPath);
  require(bool(buffer), "cannot read output dependencies");
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed) throw std::runtime_error(llvm::toString(parsed.takeError()));
  auto *object = parsed->getAsObject();
  auto *outputs = object ? object->getArray("outputs") : nullptr;
  require(outputs && outputs->size() == 25, "expected 25 Rocket output dependencies");
  SmallVector<goldengate::LocalChannelDependency> deps;
  for (auto &entry : *outputs) {
    auto *output = entry.getAsObject();
    auto name = output ? output->getString("localName") : std::nullopt;
    auto *inputs = output ? output->getArray("inputChannels") : nullptr;
    require(name && inputs, "malformed output dependency");
    goldengate::LocalChannelDependency dep{name->str(), {}, {}, {}};
    for (auto &input : *inputs) {
      auto inputName = input.getAsString();
      require(bool(inputName), "malformed dependency input");
      dep.inputChannels.push_back(inputName->str());
    }
    deps.push_back(std::move(dep));
  }
  std::string error;
  require(succeeded(goldengate::rewriteFAMEOutputValids(m, deps, error)), error.c_str());
  require(succeeded(verify(m)), "rewritten Rocket output-valid IR verification failed");
  auto emit = [&](Value dest, Value src) {
    auto f = dest.getDefiningOp<SubfieldOp>();
    auto port = f ? dyn_cast<BlockArgument>(f.getInput()) : BlockArgument();
    if (port && f.getFieldName() == "valid" &&
        m.getPortDirection(port.getArgNumber()) == Direction::Out)
      llvm::outs() << "OUTPUT_VALID " << m.getPortName(port.getArgNumber())
                   << "_valid " << predicate(m, src) << '\n';
  };
  for (auto c : m.getOps<ConnectOp>()) emit(c.getDest(), c.getSrc());
  for (auto c : m.getOps<StrictConnectOp>()) emit(c.getDest(), c.getSrc());
}
void virtualControls(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "virtual control boundary parse failed");
  auto m = model(*root);
  require(m.getNumPorts() == 6, "virtual target clock was not consumed");
  std::map<std::string, Value> registers, ports;
  for (auto r : m.getOps<RegResetOp>()) {
    registers[r.getName().str()] = r.getResult();
    if (r.getName().ends_with("_fired_0"))
      require(eval(r.getResetValue(), {}) ==
                  unsigned(r.getName() == "input_fired_0"),
              "clock association did not select the SFC reset value");
  }
  require(registers.size() == 5, "missing channel state or clock buffer");
  for (unsigned i = 0; i < m.getNumPorts(); ++i)
    ports[m.getPortName(i).str()] = m.getBodyBlock()->getArgument(i);
  auto drive = [&](Value dest) -> Value {
    auto sameDestination = [&](Value candidate) {
      if (dest == candidate)
        return true;
      auto a = dest.getDefiningOp<SubfieldOp>();
      auto b = candidate.getDefiningOp<SubfieldOp>();
      return a && b && a.getInput() == b.getInput() &&
             a.getFieldIndex() == b.getFieldIndex();
    };
    for (auto c : m.getOps<ConnectOp>())
      if (sameDestination(c.getDest()))
        return c.getSrc();
    for (auto c : m.getOps<StrictConnectOp>())
      if (sameDestination(c.getDest()))
        return c.getSrc();
    throw std::runtime_error("control destination is undriven");
  };
  Value finishing;
  for (auto w : m.getOps<WireOp>())
    if (w.getName() == "targetCycleFinishing")
      finishing = w.getResult();
  require(bool(finishing), "missing virtual cycle completion");
  InstanceOp gate;
  for (auto instance : m.getOps<InstanceOp>())
    if (instance.getName() == "target_buffer")
      gate = instance;
  require(gate && gate.getModuleName() == "AbstractClockGate" &&
              gate.getNumResults() == 3 &&
              !gate->hasAttr("goldengate.generatedClockConstraint") &&
              drive(gate.getResult(0)) == ports.at("hostClock"),
          "virtual target gate has the wrong interface or input clock");
  require(!ports.count("target"), "scalar target-clock port remains");
  auto state = *m.getOps<RegOp>().begin();
  require(state.getClockVal() == gate.getResult(2),
          "target state did not move to the gated host clock");
  RegResetOp enabled;
  for (auto r : m.getOps<RegResetOp>())
    if (r.getName() == "target_enabled")
      enabled = r;
  require(enabled.getClockVal() == ports.at("hostClock") &&
              enabled.getResetSignal() == ports.at("hostReset") &&
              eval(enabled.getResetValue(), llvm::DenseMap<Value, unsigned>()) == 0,
          "virtual enable reset/clock differs from SFC");
  for (unsigned flags = 0; flags < 8; ++flags) {
    unsigned old = flags & 1, done = (flags >> 1) & 1,
             reset = (flags >> 2) & 1;
    llvm::DenseMap<Value, unsigned> values{
        {enabled.getResult(), old}, {finishing, done},
        {ports.at("hostReset"), reset}};
    require(eval(drive(gate.getResult(1)), values) == (old & done & !reset),
            "virtual gate advances during reset/stall or before initialization");
    require((reset ? eval(enabled.getResetValue(), values)
                   : eval(drive(enabled.getResult()), values)) ==
                (reset ? 0 : done ? 1 : old),
            "virtual enable completion/hold differs from SFC");
  }
  llvm::outs() << "Matched 8 virtual-clock buffer/gate reset/hold/advance cases\n";
  for (unsigned flags = 0; flags < 256; ++flags) {
    unsigned inValid = flags & 1, virtualValid = (flags >> 1) & 1;
    unsigned fired = (flags >> 2) & 1, virtualFired = (flags >> 3) & 1;
    unsigned ready = (flags >> 4) & 1, virtualReady = (flags >> 5) & 1;
    unsigned inFired = (flags >> 6) & 1, virtualInFired = (flags >> 7) & 1;
    unsigned outValid = inValid & !fired,
             virtualOutValid = virtualValid & !virtualFired;
    unsigned complete = inValid & virtualValid & (fired | ready) &
                        (virtualFired | virtualReady);
    llvm::DenseMap<Value, unsigned> values{
        {finishing, complete},
        {registers.at("input_fired_0"), inFired},
        {registers.at("virtualInput_fired_0"), virtualInFired},
        {registers.at("output_fired_0"), fired},
        {registers.at("virtualOutput_fired_0"), virtualFired}};
    for (auto f : m.getOps<SubfieldOp>()) {
      if (f.getFieldName() == "valid") {
        if (f.getInput() == ports.at("input_sink"))
          values[f.getResult()] = inValid;
        if (f.getInput() == ports.at("virtualInput_sink"))
          values[f.getResult()] = virtualValid;
        if (f.getInput() == ports.at("output_source"))
          values[f.getResult()] = outValid;
        if (f.getInput() == ports.at("virtualOutput_source"))
          values[f.getResult()] = virtualOutValid;
      }
      if (f.getFieldName() == "ready") {
        if (f.getInput() == ports.at("output_source"))
          values[f.getResult()] = ready;
        if (f.getInput() == ports.at("virtualOutput_source"))
          values[f.getResult()] = virtualReady;
        if (f.getInput() == ports.at("input_sink"))
          values[f.getResult()] = complete & !inFired;
        if (f.getInput() == ports.at("virtualInput_sink"))
          values[f.getResult()] = complete & !virtualInFired;
      }
    }
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
      values[registers.at("target_enabled")] = enabled;
      auto transition = [&](const char *name,
                            unsigned old,
                            unsigned handshake,
                            unsigned enable) {
        require(eval(drive(registers.at(name)), values) ==
                    (complete ? !enable : old | handshake),
                "compiler-emitted fired transition has wrong clock enable");
      };
      transition("input_fired_0", inFired, inValid & (complete & !inFired), 1);
      transition("virtualInput_fired_0",
                 virtualInFired,
                 virtualValid & (complete & !virtualInFired),
                 1);
      transition("output_fired_0", fired, ready & outValid, enabled);
      transition("virtualOutput_fired_0",
                 virtualFired,
                 virtualReady & virtualOutValid,
                 1);
    }
    require(eval(drive(finishing), values) == complete,
            "virtual finishing differs from SFC topRules");
    for (auto f : m.getOps<SubfieldOp>()) {
      Value port = f.getInput();
      if (f.getFieldName() == "valid" && port == ports.at("output_source"))
        require(eval(drive(f.getResult()), values) == outValid,
                "clocked output valid differs");
      if (f.getFieldName() == "valid" &&
          port == ports.at("virtualOutput_source"))
        require(eval(drive(f.getResult()), values) == virtualOutValid,
                "clockless output valid differs");
      if (f.getFieldName() == "ready" && port == ports.at("input_sink"))
        require(eval(drive(f.getResult()), values) == (complete & !inFired),
                "clocked input ready differs");
      if (f.getFieldName() == "ready" && port == ports.at("virtualInput_sink"))
        require(eval(drive(f.getResult()), values) ==
                    (complete & !virtualInFired),
                "clockless input ready differs");
    }
  }
  llvm::outs() << "Passed 256 compiler-emitted virtual channel-control cases "
                  "and 2048 fired transitions\n";
}
// A hub clock channel and a non-hub data channel deliberately share "tick".
// Evaluate emitted SSA controls against FAMETransformer's per-model rules.
void mixedControls(MLIRContext &context, const char *path,
                   bool inputOnly = false, llvm::StringRef onlyModel = {}) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "mixed control boundary parse failed");
  unsigned models = 0, cases = 0, declarations = 0;
  root->walk([&](FExtModuleOp ext) {
    if (ext.getName() == "AbstractClockGate")
      ++declarations;
  });
  require(declarations == 1, "mixed models did not share the clock-gate symbol");
  root->walk([&](FModuleOp m) {
    if (!onlyModel.empty() && m.getName() != onlyModel)
      return;
    bool hub = m.getName() == "Hub";
    if (!hub && m.getName() != "Other")
      return;
    ++models;
    std::map<std::string, Value> ports;
    std::map<std::string, RegResetOp> registers;
    for (unsigned i = 0; i < m.getNumPorts(); ++i)
      ports[m.getPortName(i).str()] = m.getBodyBlock()->getArgument(i);
    require(!ports.count("target"), "virtual target clock was not consumed");
    for (auto reg : m.getOps<RegResetOp>())
      registers[reg.getName().str()] = reg;
    auto inputName = hub ? "input" : "tick";
    require(registers.count(std::string(inputName) + "_fired_0"),
            "input-only model was skipped because it has no output dependencies");
    auto inFired = registers.at(std::string(inputName) + "_fired_0");
    RegResetOp outFired;
    if (!inputOnly)
      outFired = registers.at("output_fired_0");
    auto enabled = registers.at(hub ? "tick_enabled" : "target_enabled");
    require(registers.size() == (inputOnly ? 2 : 3) &&
                eval(inFired.getResetValue(), {}) == unsigned(hub) &&
                (inputOnly || eval(outFired.getResetValue(), {}) == 0),
            "local clock association did not select channel reset values");
    Value finishing;
    for (auto wire : m.getOps<WireOp>())
      if (wire.getName() == "targetCycleFinishing")
        finishing = wire.getResult();
    auto drive = [&](Value dest) -> Value {
      auto same = [&](Value candidate) {
        if (candidate == dest)
          return true;
        auto a = dest.getDefiningOp<SubfieldOp>();
        auto b = candidate.getDefiningOp<SubfieldOp>();
        return a && b && a.getInput() == b.getInput() &&
               a.getFieldIndex() == b.getFieldIndex();
      };
      for (auto c : m.getOps<StrictConnectOp>())
        if (same(c.getDest()))
          return c.getSrc();
      for (auto c : m.getOps<ConnectOp>())
        if (same(c.getDest()))
          return c.getSrc();
      throw std::runtime_error("mixed control has no driver");
    };
    InstanceOp gate;
    for (auto instance : m.getOps<InstanceOp>())
      if (instance.getModuleName() == "AbstractClockGate")
        gate = instance;
    require(gate && finishing &&
                drive(gate.getResult(0)) == ports.at("hostClock") &&
                enabled.getClockVal() == ports.at("hostClock") &&
                enabled.getResetSignal() == ports.at("hostReset"),
            "mixed model clock buffer has wrong host controls");
    auto state = *m.getOps<RegOp>().begin();
    require(state.getClockVal() == gate.getResult(2),
            "mixed model target state uses an ungated clock");
    for (unsigned flags = 0; flags < (inputOnly ? 64 : 256); ++flags) {
      unsigned valid = flags & 1, fired = (flags >> 1) & 1,
               ready = (flags >> 2) & 1, oldInput = (flags >> 3) & 1,
               clockValid = (flags >> 4) & 1, token = (flags >> 5) & 1,
               oldEnable = (flags >> 6) & 1, reset = (flags >> 7) & 1;
      if (inputOnly) {
        oldInput = (flags >> 1) & 1;
        clockValid = (flags >> 2) & 1;
        token = (flags >> 3) & 1;
        oldEnable = (flags >> 4) & 1;
        reset = (flags >> 5) & 1;
      }
      unsigned outValid = valid & !fired;
      unsigned allReady = inputOnly ? valid : valid & (fired | ready);
      unsigned done = allReady & (hub ? clockValid : 1);
      unsigned inputReady = done & !oldInput;
      llvm::DenseMap<Value, unsigned> values{
          {inFired.getResult(), oldInput},
          {enabled.getResult(), oldEnable}, {ports.at("hostReset"), reset}};
      if (outFired)
        values[outFired.getResult()] = fired;
      for (auto field : m.getOps<SubfieldOp>()) {
        auto port = field.getInput();
        auto name = field.getFieldName();
        if (port == ports.at(std::string(inputName) + "_sink")) {
          if (name == "valid") values[field.getResult()] = valid;
          if (name == "ready") values[field.getResult()] = inputReady;
        }
        if (!inputOnly && port == ports.at("output_source")) {
          if (name == "valid") values[field.getResult()] = outValid;
          if (name == "ready") values[field.getResult()] = ready;
        }
        if (hub && port == ports.at("tick_sink")) {
          if (name == "valid") values[field.getResult()] = clockValid;
          if (name == "bits") values[field.getResult()] = token;
        }
      }
      require(eval(drive(finishing), values) == done,
              "completion selected another model's clock channel");
      for (auto field : m.getOps<SubfieldOp>()) {
        if (hub && field.getInput() == ports.at("tick_sink") &&
            field.getFieldName() == "ready")
          require(eval(drive(field.getResult()), values) == allReady,
                  "hub clock ready incorrectly depends on clock valid");
        if (!inputOnly && field.getInput() == ports.at("output_source") &&
            field.getFieldName() == "valid")
          require(eval(drive(field.getResult()), values) == outValid,
                  "mixed model output dependency differs from SFC");
        if (field.getInput() == ports.at(std::string(inputName) + "_sink") &&
            field.getFieldName() == "ready")
          require(eval(drive(field.getResult()), values) == inputReady,
                  "same-named data channel lost its ready control");
      }
      unsigned nextInput = reset ? eval(inFired.getResetValue(), values)
                                : eval(drive(inFired.getResult()), values);
      require(nextInput == (reset ? unsigned(hub)
                           : done ? !(hub ? token : 1)
                                  : oldInput | (valid & inputReady)),
              "mixed model fired transition has wrong clock association");
      if (outFired) {
        unsigned nextOutput = reset ? eval(outFired.getResetValue(), values)
                                    : eval(drive(outFired.getResult()), values);
        require(nextOutput == (reset ? 0 : done ? !oldEnable
                                               : fired | (ready & outValid)),
                "mixed model output fired transition differs from SFC");
      }
      require(eval(drive(gate.getResult(1)), values) ==
                  (oldEnable & done & !reset),
              "mixed model gate ignores local completion or reset");
      llvm::outs() << (inputOnly ? "INPUT_ONLY_CASE " : "MIXED_CASE ")
                   << m.getName() << " " << flags << " "
                   << eval(drive(finishing), values) << " " << allReady;
      if (inputOnly)
        llvm::outs() << " " << inputReady << " " << nextInput << " "
                     << eval(drive(gate.getResult(1)), values);
      llvm::outs() << "\n";
      ++cases;
    }
  });
  require(models == (onlyModel.empty() ? 2 : 1) && succeeded(verify(*root)),
          "mixed circuit models missing or invalid");
  llvm::outs() << "Passed " << cases
               << (inputOnly ? " input-only" : " mixed")
               << " hub/virtual control cases\n";
}
void selectedControls(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "selected model boundary parse failed");
  bool found = false;
  root->walk([&](FModuleOp m) {
    if (m.getName() != "Hub")
      return;
    found = true;
    for (auto reg : m.getOps<RegResetOp>())
      require(reg.getName() == "tick_enabled",
              "unselected model acquired channel fired registers");
    for (auto wire : m.getOps<WireOp>())
      if (wire.getName() == "targetCycleFinishing")
        require(eval(wire.getResult(), {}) == 0,
                "unselected model completion was rewritten");
  });
  require(found, "unselected model missing");
  mixedControls(context, path, true, "Other");
}
// Emit the actual CIRCT buffer/gate truth table for comparison with the SFC
// RTL boundary. Token substitution also tests the virtual constant-one case.
void clockControls(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "clock control boundary parse failed");
  unsigned gates = 0;
  root->walk([&](FModuleOp m) {
    for (auto gate : m.getOps<InstanceOp>()) {
      if (gate.getModuleName() != "AbstractClockGate")
        continue;
      require(gate.getName().ends_with("_buffer"), "unexpected clock gate name");
      auto stem = gate.getName().drop_back(7); // _buffer
      RegResetOp enabled;
      Value finishing, hostClock, hostReset;
      for (unsigned i = 0; i < m.getNumPorts(); ++i) {
        if (m.getPortName(i) == "hostClock")
          hostClock = m.getBodyBlock()->getArgument(i);
        if (m.getPortName(i) == "hostReset")
          hostReset = m.getBodyBlock()->getArgument(i);
      }
      for (auto r : m.getOps<RegResetOp>())
        if (r.getName() == (stem + "_enabled").str())
          enabled = r;
      for (auto w : m.getOps<WireOp>())
        if (w.getName() == "targetCycleFinishing")
          finishing = w.getResult();
      auto drive = [&](Value dest) -> Value {
        for (auto c : m.getOps<StrictConnectOp>())
          if (c.getDest() == dest)
            return c.getSrc();
        for (auto c : m.getOps<ConnectOp>())
          if (c.getDest() == dest)
            return c.getSrc();
        throw std::runtime_error("clock control has no driver");
      };
      require(enabled && finishing && hostClock && hostReset &&
                  drive(gate.getResult(0)) == hostClock &&
                  enabled.getClockVal() == hostClock &&
                  enabled.getResetSignal() == hostReset,
              "clock buffer/gate host controls differ");
      Value next = drive(enabled.getResult());
      auto mux = next.getDefiningOp<MuxPrimOp>();
      require(mux && mux.getSel() == finishing &&
                  mux.getLow() == enabled.getResult(),
              "clock buffer does not hold until completion");
      bool virtualToken = bool(mux.getHigh().getDefiningOp<ConstantOp>());
      for (unsigned flags = 0; flags < 16; ++flags) {
        unsigned old = flags & 1, done = (flags >> 1) & 1,
                 reset = (flags >> 2) & 1, token = (flags >> 3) & 1;
        llvm::DenseMap<Value, unsigned> values{
            {enabled.getResult(), old}, {finishing, done}, {hostReset, reset}};
        if (!virtualToken)
          values[mux.getHigh()] = token;
        unsigned actualNext = reset ? eval(enabled.getResetValue(), values)
                                    : eval(next, values);
        unsigned ce = eval(drive(gate.getResult(1)), values);
        require(actualNext == (reset ? 0 : done ? (virtualToken ? 1 : token) : old)
                    && ce == (old & done & !reset),
                "clock buffer/gate differs from SFC");
        llvm::outs() << "CLOCK_CASE " << m.getName() << " " << flags << " "
                     << actualNext << " " << ce << "\n";
      }
      ++gates;
    }
  });
  require(gates != 0, "no clock gate found at boundary");
  require(succeeded(verify(*root)), "clock control boundary does not verify");
}
void boundary(MLIRContext &context, const char *path) {
  auto root = parseSourceFile<ModuleOp>(path, &context);
  require(bool(root), "candidate boundary parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp m;
  for (auto candidate : circuit.getOps<FModuleOp>())
    if (candidate.getName() == "FireSim")
      m = candidate;
  require(bool(m), "FireSim model missing at boundary");
  Value clockPort, outputEnable;
  for (unsigned i = 0; i < m.getNumPorts(); ++i)
    if (m.getPortName(i) == "clockBridge_clocks_0_sink")
      clockPort = m.getBodyBlock()->getArgument(i);
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName() == "clockBridge_clocks_0_enabled")
      outputEnable = reg.getResult();
  require(clockPort && outputEnable, "missing target clock controls");
  OpBuilder b(&m.getBodyBlock()->front());
  Value inputEnable = b.create<AsUIntPrimOp>(
      m.getLoc(), b.create<SubfieldOp>(m.getLoc(), clockPort, "bits"));
  SmallVector<goldengate::FAMEFiredChannel> cs;
  for (unsigned i = 0; i < m.getNumPorts(); ++i) {
    auto name = m.getPortName(i);
    if (name == "clockBridge_clocks_0_sink")
      continue;
    if (name.ends_with("_sink"))
      cs.push_back({name.drop_back(5).str(), true, inputEnable});
    if (name.ends_with("_source"))
      cs.push_back({name.drop_back(7).str(), false, outputEnable});
  }
  require(cs.size() == 42, "unexpected Rocket data channel count");
  std::string error;
  // Recreate the native host declarations at the saved model boundary. Keep
  // old values alive until their uses can be transferred, reserving different
  // names so that creation sees the same target namespace as before FAME.
  SmallVector<std::pair<std::string, RegResetOp>> oldRegisters;
  for (const auto &ch : cs)
    for (auto reg : m.getOps<RegResetOp>())
      if (reg.getName() == ch.name + "_fired_0") {
        oldRegisters.push_back({ch.name, reg});
        reg.setName((reg.getName() + "_oracle").str());
        reg->removeAttr(goldengate::fameFiredChannelAttr);
      }
  require(oldRegisters.size() == 42, "missing saved Rocket fired declarations");
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)), error.c_str());
  goldengate::FAMEFiredRegisterIndex recreated;
  require(succeeded(recreated.collect(m, error)), error.c_str());
  for (auto &[name, reg] : oldRegisters) {
    SmallVector<Operation *> connects;
    for (auto *user : reg.getResult().getUsers())
      if ((isa<ConnectOp, StrictConnectOp>(user)) && user->getOperand(0) == reg.getResult())
        connects.push_back(user);
    for (auto *connect : connects)
      connect->erase();
    reg.getResult().replaceAllUsesWith(recreated.lookup(name));
    reg->erase();
  }
  auto before = dump(m);
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)) &&
              before == dump(m), "Rocket recreation is not idempotent");
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)),
          error.c_str());
  SmallVector<std::string> inputs, outputs;
  for (const auto &ch : cs)
    (ch.isInput ? inputs : outputs).push_back(ch.name);
  require(succeeded(goldengate::rewriteFAMEFinishing(
              m, inputs, outputs, "clockBridge_clocks_0", error)),
          error.c_str());
  auto emitPredicate = [&](Value dest, Value src) {
    if (auto w = dest.getDefiningOp<WireOp>();
        w && w.getName() == "targetCycleFinishing")
      llvm::outs() << "COMPLETION " << predicate(m, src) << "\n";
    if (auto f = dest.getDefiningOp<SubfieldOp>();
        f && f.getInput() == clockPort && f.getFieldName() == "ready")
      llvm::outs() << "CLOCK_READY " << predicate(m, src) << "\n";
  };
  for (auto c : m.getOps<ConnectOp>())
    emitPredicate(c.getDest(), c.getSrc());
  for (auto c : m.getOps<StrictConnectOp>())
    emitPredicate(c.getDest(), c.getSrc());
  require(succeeded(verify(m)), "candidate model failed verification");
  for (auto reg : m.getOps<RegResetOp>())
    if (reg.getName().ends_with("_fired_0")) {
      require(reg.getClockVal() == m.getBodyBlock()->getArgument(0) &&
                  reg.getResetSignal() == m.getBodyBlock()->getArgument(1),
              "boundary host controls differ");
      llvm::outs() << "BOUNDARY " << reg.getName() << " "
                   << eval(reg.getResetValue(),
                           llvm::DenseMap<Value, unsigned>())
                   << "\n";
    }
  unsigned cases = 0;
  for (const auto &ch : cs) {
    RegResetOp reg;
    Value port;
    for (auto r : m.getOps<RegResetOp>())
      if (r.getName() == ch.name + "_fired_0")
        reg = r;
    for (unsigned i = 0; i < m.getNumPorts(); ++i)
      if (m.getPortName(i) == ch.name + (ch.isInput ? "_sink" : "_source"))
        port = m.getBodyBlock()->getArgument(i);
    Value next;
    for (auto c : m.getOps<ConnectOp>())
      if (c.getDest() == reg.getResult())
        next = c.getSrc();
    for (auto c : m.getOps<StrictConnectOp>())
      if (c.getDest() == reg.getResult())
        next = c.getSrc();
    Value finishing;
    for (auto w : m.getOps<WireOp>())
      if (w.getName() == "targetCycleFinishing")
        finishing = w.getResult();
    require(reg && port && next && finishing, "incomplete boundary transition");
    for (unsigned flags = 0; flags < 64; ++flags) {
      unsigned old = flags & 1, ready = (flags >> 1) & 1,
               valid = (flags >> 2) & 1;
      unsigned done = (flags >> 3) & 1, enable = (flags >> 4) & 1,
               hostReset = (flags >> 5) & 1;
      llvm::DenseMap<Value, unsigned> values{{reg.getResult(), old},
                                             {finishing, done},
                                             {ch.clockDomainEnable, enable}};
      for (auto f : m.getOps<SubfieldOp>())
        if (f.getInput() == port) {
          if (f.getFieldName() == "ready")
            values[f.getResult()] = ready;
          if (f.getFieldName() == "valid")
            values[f.getResult()] = valid;
        }
      unsigned actual =
          hostReset ? eval(reg.getResetValue(), values) : eval(next, values);
      require(actual == (hostReset ? unsigned(ch.isInput)
                         : done    ? !enable
                                   : old | (ready & valid)),
              "Rocket transition/reset priority differs from SFC");
      ++cases;
    }
  }
  llvm::outs() << "Passed " << cases
               << " Rocket reset/completion/handshake combinations\n";
  llvm::outs() << "Validated all 42 Rocket fired-register reset contracts and "
                  "rewrote transitions\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context);
    rejections(context);
    collidingIdentities(context);
    finishingBehavior(context);
    noDataFinishingRejections(context);
    if (argc == 4 && std::string(argv[1]) == "--output-valids")
      outputValidsBoundary(context, argv[2], argv[3]);
    else if (argc == 3 && std::string(argv[1]) == "--virtual-controls")
      virtualControls(context, argv[2]);
    else if (argc == 3 && std::string(argv[1]) == "--clock-controls")
      clockControls(context, argv[2]);
    else if (argc == 3 && std::string(argv[1]) == "--mixed-controls")
      mixedControls(context, argv[2]);
    else if (argc == 3 && std::string(argv[1]) == "--input-only-controls")
      mixedControls(context, argv[2], true);
    else if (argc == 3 && std::string(argv[1]) == "--selected-controls")
      selectedControls(context, argv[2]);
    else if (argc == 2)
      boundary(context, argv[1]);
    else
      require(argc == 1,
              "usage: FAMEFiredStateTest [candidate.mlir | --virtual-controls "
              "candidate.mlir | --clock-controls candidate.mlir | "
              "--mixed-controls candidate.mlir | "
              "--input-only-controls candidate.mlir | "
              "--selected-controls candidate.mlir | --output-valids "
              "candidate.mlir dependencies.json]");
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
