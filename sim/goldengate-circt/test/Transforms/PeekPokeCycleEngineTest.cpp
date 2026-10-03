// See LICENSE for license details.
#include "goldengate/PeekPokeCycleEngine.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) { if (!ok) throw std::runtime_error(message.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) ++regular;
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 0; }
    require(regular == 0 && reset == 4, "wrong engine reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & (bits.getHi() - bits.getLo() == 63 ? UINT64_MAX : (uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = eval(op->getOperand(0)) >= eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned maximum = 2,
                            unsigned fieldWidth = 1, unsigned latency = 0,
                            bool hasSource = false) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGUARTBridgeControlWrapper" {
      firrtl.module @GGUARTBridgeControlWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %resetToken: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  auto dict = [&](std::initializer_list<NamedAttribute> values) { return b.getDictionaryAttr(values); };
  auto str = [&](llvm::StringRef name, llvm::StringRef value) { return b.getNamedAttr(name, b.getStringAttr(value)); };
  auto key = dict({str("class", "firesim.lib.bridges.PeekPokeKey"),
      b.getNamedAttr("maxChannelDecoupling", b.getI64IntegerAttr(maximum)),
      b.getNamedAttr("peeks", b.getArrayAttr({})),
      b.getNamedAttr("pokes", b.getArrayAttr({dict({str("name", "reset"),
          b.getNamedAttr("fieldWidth", b.getI64IntegerAttr(fieldWidth)),
          b.getNamedAttr("tpe", dict({str("typeString", "UInt")}))})}))});
  auto bridge = dict({str("class", "firesim.lib.bridgeutils.BridgeIOAnnotation"),
      str("widgetClass", "midas.widgets.PeekPokeBridgeModule"),
      b.getNamedAttr("widgetConstructorKey", key),
      b.getNamedAttr("channelMapping", dict({str("reset", "peekPokeBridge_reset")})),
      str("target", "~FireSim|FireSim>peekPokeBridge")});
  NamedAttrList channel(dict({str("class", "midas.passes.fame.FAMEChannelConnectionAnnotation"),
      str("globalName", "peekPokeBridge_reset"),
      b.getNamedAttr("channelInfo", dict({str("class", "midas.passes.fame.PipeChannel"),
          b.getNamedAttr("latency", b.getI64IntegerAttr(latency))})),
      b.getNamedAttr("sinks", b.getArrayAttr({b.getStringAttr("~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>resetToken.bits")}))}));
  if (hasSource) channel.set("sources", b.getArrayAttr({b.getStringAttr("~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>other")}));
  auto copied = dict({str("class", "test.Annotation"), str("target", "~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>other")});
  c->setAttr("rawAnnotations", b.getArrayAttr({bridge, channel.getDictionary(&context), copied}));
  return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addPeekPokeCycleEngine(c, error)), error);
  require(succeeded(verify(*root)), "engine IR verification failed");
  Interpreter sim(named(c, "GGPeekPokeCycleEngine"));
  std::map<std::string, Value> regs;
  for (auto r : sim.module.getOps<RegResetOp>()) regs[r.getName().str()] = r.getResult();
  auto read = [&](llvm::StringRef name) { return sim.state.lookup(regs.at(name.str())); };
  std::mt19937_64 random(154);
  unsigned samples = 0;
  auto check = [&](uint32_t horizon, unsigned ahead, unsigned credit, uint64_t cycles, unsigned flags) {
    sim.state[regs.at("cycleHorizon")] = horizon; sim.state[regs.at("cyclesAhead")] = ahead;
    sim.state[regs.at("wordsReceived")] = credit; sim.state[regs.at("tCycle")] = cycles;
    bool reset = flags & 1, ready = flags & 2, poke = flags & 4, stepValid = flags & 8, bits = flags & 16;
    uint32_t step = random();
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    auto put = [&](unsigned i, llvm::StringRef path, uint64_t value) { sim.memo[sim.key(sim.arg(i)) + "." + path.str()] = value; };
    put(2, "reset.ready", ready); put(3, "step.valid", stepValid); put(3, "step.bits", step);
    put(3, "poke", poke); put(3, "resetValue", bits);
    bool done = horizon == 0, valid = credit || (ahead < 2 && ahead < horizon);
    bool sent = ready && valid, consumed = horizon != 0 && (ahead != 0 || sent);
    require(sim.output(2, "reset.valid") == valid && sim.output(2, "reset.bits") == bits, "token mismatch");
    require(sim.output(3, "step.ready") == done && sim.output(3, "done") == done &&
            sim.output(3, "precisePeekable") == done && sim.output(3, "tCycle") == cycles, "control mismatch");
    unsigned expectedAhead = ahead;
    if (sent != consumed) {
      if (sent && ahead < 2) ++expectedAhead;
      if (consumed && ahead != 0) --expectedAhead;
    }
    uint32_t expectedHorizon = done && stepValid ? step : horizon - unsigned(consumed);
    unsigned expectedCredit = poke ? credit ^ 1 : sent ? 0 : credit;
    sim.edge();
    require(read("cycleHorizon") == (reset ? 0 : expectedHorizon), "horizon next state mismatch");
    require(read("cyclesAhead") == (reset ? 0 : expectedAhead), "decoupling next state mismatch");
    require(read("wordsReceived") == (reset ? 0 : expectedCredit), "poke priority mismatch");
    require(read("tCycle") == (reset ? 0 : cycles + uint64_t(consumed)), "cycle next state mismatch");
    ++samples;
  };
  for (uint32_t horizon : {0U, 1U, 2U, 3U, 4U, 0xFFFFFFFFU})
    for (unsigned ahead = 0; ahead < 4; ++ahead)
      for (unsigned credit = 0; credit < 2; ++credit)
        for (unsigned flags = 0; flags < 32; ++flags)
          for (uint64_t cycles : {uint64_t(0), uint64_t(0xFFFFFFFF), UINT64_MAX})
            check(horizon, ahead, credit, cycles, flags);
  // Carry state through a sustained random sequence, with long stalls and
  // repeated pokes while stepping. A poke is allowed to bypass full/horizon.
  for (unsigned i = 0; i < 20000; ++i)
    check(read("cycleHorizon"), read("cyclesAhead"), read("wordsReceived"), read("tCycle"),
          unsigned(random()) & (i % 127 == 0 ? 31 : 30));
  llvm::outs() << samples << " native cycle scheduling comparisons passed\n";
}
std::string print(ModuleOp root) { std::string s; llvm::raw_string_ostream out(s); root.print(out); return s; }
void mappingAndRejection(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto key = cast<DictionaryAttr>(before[0]).get("widgetConstructorKey");
  require(succeeded(goldengate::addPeekPokeCycleEngine(c, error)), error);
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == before.size(), "annotation loss");
  require(cast<DictionaryAttr>(raw[0]).get("widgetConstructorKey") == key, "constructor changed");
  for (auto [i, a] : llvm::enumerate(raw))
    require(cast<DictionaryAttr>(a).get("class") == cast<DictionaryAttr>(before[i]).get("class"), "class changed");
  auto source = cast<StringAttr>(cast<DictionaryAttr>(raw[1]).getAs<ArrayAttr>("sources")[0]);
  auto resolved = goldengate::resolveAnnotationTarget(c, source.getValue(), error);
  require(resolved && resolved->module.getModuleName() == "GGPeekPokeCycleEngine", "source target unresolved");
  auto sink = cast<StringAttr>(cast<DictionaryAttr>(raw[1]).getAs<ArrayAttr>("sinks")[0]);
  resolved = goldengate::resolveAnnotationTarget(c, sink.getValue(), error);
  require(resolved && resolved->module.getModuleName() == "GGUARTBridgeControlWrapper", "removed sink moved outside queue wrapper");
  auto copied = cast<DictionaryAttr>(raw[2]).getAs<StringAttr>("target");
  resolved = goldengate::resolveAnnotationTarget(c, copied.getValue(), error);
  require(resolved && resolved->module.getModuleName() == "GGPeekPokeCycleWrapper", "copied port not transferred");
  auto outer = named(c, "GGPeekPokeCycleWrapper");
  require(outer.getNumPorts() == 4 && outer.getPortName(3) == "peekPokeBridge_cycle", "wrong wrapper boundary");
  unsigned producers = 0, connections = 0;
  outer.walk([&](InstanceOp i) { producers += i.getModuleName() == "GGPeekPokeCycleEngine"; });
  outer.walk([&](ConnectOp) { ++connections; });
  require(producers == 1 && connections == 5, "token/control wrapper wiring missing");
  auto snapshot = print(*root);
  require(failed(goldengate::addPeekPokeCycleEngine(c, error)) && print(*root) == snapshot, "repeat pass mutated IR");
  for (unsigned mode = 0; mode < 6; ++mode) {
    auto bad = fixture(context, mode == 0 ? 1 : 2, mode == 1 ? 2 : 1, mode == 2 ? 1 : 0, mode == 3);
    auto bc = *bad->getOps<CircuitOp>().begin();
    if (mode == 4) bc->removeAttr("rawAnnotations");
    if (mode == 5) {
      auto annos = bc->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> all(annos.begin(), annos.end());
      all.push_back(annos[0]); OpBuilder b(&context); bc->setAttr("rawAnnotations", b.getArrayAttr(all));
    }
    auto saved = print(*bad);
    require(failed(goldengate::addPeekPokeCycleEngine(bc, error)) && !error.empty() && print(*bad) == saved,
            "unsupported boundary mutated IR");
  }
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mappingAndRejection(context);
    llvm::outs() << "PeekPoke cycle engine: wrapper, annotation targets, constructor and atomic rejection passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
