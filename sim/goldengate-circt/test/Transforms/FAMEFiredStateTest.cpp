// See LICENSE for license details.
#include "goldengate/FAMEFiredState.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  return parseSourceString<ModuleOp>(R"mlir(
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
  )mlir",
                                     &context);
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
  auto before = dump(m);
  std::string error;
  require(succeeded(goldengate::ensureFAMEFiredRegisters(m, cs, error)),
          error.c_str());
  require(before == dump(m), "oracle boundary register declaration changed");
  require(succeeded(goldengate::rewriteFAMEFiredStates(m, cs, error)),
          error.c_str());
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
    if (argc == 2)
      boundary(context, argv[1]);
    else
      require(argc == 1, "usage: FAMEFiredStateTest [candidate.mlir]");
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
