// See LICENSE for license details.
#include "goldengate/ResetPulseBridge.h"
#include "goldengate/AnnotationClasses.h"
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
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp circuit, llvm::StringRef name) {
  for (auto m : circuit.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
#include "llvm/Support/MathExtras.h"
OwningOpRef<ModuleOp> fixture(MLIRContext &context, bool activeHigh = true,
                             unsigned maximum = 1023, unsigned bad = 0,
                             bool resetFirst = false) {
  std::string resetPort = "in %resetTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>";
  std::string hostPorts = "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>";
  auto root = parseSourceString<ModuleOp>(
    "module { firrtl.circuit \"GGClockBridgeControlWrapper\" { firrtl.module @GGClockBridgeControlWrapper(" +
    (resetFirst ? resetPort + ", " + hostPorts : hostPorts + ", " + resetPort) +
    ", out %data: !firrtl.uint<8>) {} } }", &context);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto key = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("firesim.lib.bridges.ResetPulseBridgeParameters")),
    b.getNamedAttr("activeHigh", b.getBoolAttr(activeHigh)),
    b.getNamedAttr("maxPulseLength", b.getI64IntegerAttr(bad == 2 ? 0 : maximum)),
    b.getNamedAttr("defaultPulseLength", b.getI64IntegerAttr(bad == 3 ? maximum + 1 : maximum / 2))});
  auto bridge = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
    b.getNamedAttr("target", b.getStringAttr("~Original|Original>resetBridge")),
    b.getNamedAttr("widgetClass", b.getStringAttr("midas.widgets.ResetPulseBridgeModule")),
    b.getNamedAttr("widgetConstructorKey", key),
    b.getNamedAttr("channelMapping", b.getDictionaryAttr({
      b.getNamedAttr("reset", b.getStringAttr(bad == 4 ? "other" : "resetBridge_reset"))}))});
  auto info = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
    b.getNamedAttr("latency", b.getI64IntegerAttr(bad == 5 ? 1 : 0))});
  NamedAttrList channel;
  channel.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
  channel.set("globalName", b.getStringAttr("resetBridge_reset"));
  channel.set("channelInfo", info);
  channel.set("clock", b.getStringAttr("~GGClockBridgeControlWrapper|GGClockBridgeControlWrapper>hostClock"));
  channel.set("sinks", b.getArrayAttr({b.getStringAttr(
      bad == 6 ? "~GGClockBridgeControlWrapper|GGClockBridgeControlWrapper>resetTokens.valid" :
                 "~GGClockBridgeControlWrapper|GGClockBridgeControlWrapper>resetTokens.bits")}));
  if (bad == 7) channel.set("sources", channel.get("sinks"));
  auto other = b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
    b.getNamedAttr("target", b.getStringAttr("~GGClockBridgeControlWrapper|GGClockBridgeControlWrapper>data"))});
  SmallVector<Attribute> annotations{channel.getDictionary(&context), other};
  if (bad != 1) annotations.push_back(bridge);
  if (bad == 8) annotations.push_back(bridge);
  if (bad == 9) annotations.push_back(channel.getDictionary(&context));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (bad == 10) {
    auto inner = named(c, "GGClockBridgeControlWrapper");
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto use = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Other"), inner.getConventionAttr(), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(use.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), inner, "use");
  }
  if (bad == 11) {
    auto inner = named(c, "GGClockBridgeControlWrapper");
    auto names = SmallVector<Attribute>(inner.getPortNamesAttr().begin(), inner.getPortNamesAttr().end());
    names[3] = b.getStringAttr("resetBridge_mcr"); inner.setPortNamesAttr(b.getArrayAttr(names));
  }
  return root;
}
// Evaluate emitted FIRRTL, canonicalizing repeated accesses of bundle fields.
// State advances simultaneously; RegOp has no reset, RegResetOp has precedence.
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>()) return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp();
    uint64_t n;
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) {
      unsigned w = bits.getHi() - bits.getLo() + 1;
      n = (eval(bits.getInput()) >> bits.getLo()) & (w == 64 ? ~uint64_t(0) : (uint64_t(1) << w) - 1);
    } else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto a : module.getOps<AssertOp>())
      require(!eval(a.getEnable()) || eval(a.getPredicate()), "register permission assertion fired");
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &context, bool activeHigh, unsigned maximum) {
  auto root = fixture(context, activeHigh, maximum);
  auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addResetPulseBridge(c, error)), error);
  require(succeeded(verify(*root)), "reset bridge IR verification failed");
  auto producer = named(c, "GGResetPulseBridge");
  Interpreter sim(producer);
  unsigned width = llvm::Log2_64_Ceil(uint64_t(maximum) + 1);
  uint32_t mask = (uint32_t(1) << width) - 1;
  uint32_t remaining = mask; bool initialized = true;
  for (auto r : producer.getOps<RegOp>()) sim.state[r.getResult()] = remaining;
  for (auto r : producer.getOps<RegResetOp>()) sim.state[r.getResult()] = initialized;
  OpBuilder b(&context); b.setInsertionPointToEnd(producer.getBodyBlock());
  auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(producer.getLoc(), v, n); };
  auto token = field(sim.arg(2), "reset");
  auto tokenValid = field(token, "valid"), tokenBits = field(token, "bits");
  std::mt19937 random(149);
  unsigned writesDuringFire = 0, stalls = 0, accepted = 0, zeroTokens = 0, directedPulseTokens = 0;
  for (unsigned cycle = 0; cycle < 20000; ++cycle) {
    bool reset = cycle == 0 || cycle % 719 == 0;
    bool ready = cycle % 57 >= 12 && (random() & 1);
    bool writePulse = cycle % 47 == 1, writeInit = cycle % 97 == 1;
    uint32_t pulseData = random(), initData = random();
    // Directed programming and extended stalls verify exact pulse duration,
    // saturation at zero, write priority, and register updates during reset.
    if (cycle == 0) writePulse = writeInit = true, pulseData = mask, initData = 1;
    if (cycle == 1) writePulse = writeInit = true, pulseData = maximum, initData = 1;
    if (cycle >= 2 && cycle < 3000) writePulse = writeInit = reset = false;
    if (cycle >= 2 && cycle < 1100) ready = true;
    if (cycle == 3000) ready = true, writePulse = true, pulseData = 1;
    if (cycle == 3001) ready = true, writePulse = true, pulseData = mask + 2;
    if (cycle == 3002) ready = true, writePulse = true, pulseData = 0;
    if (cycle == 3003) ready = true, writePulse = true, pulseData = 0xffffffff;
    if (cycle >= 3000 && cycle % 719 == 0) writePulse = writeInit = true, pulseData = 7, initData = 1;
    sim.memo.clear();
    sim.memo[sim.key(sim.arg(0))] = 0; sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(token) + ".ready"] = ready;
    auto bank = sim.key(sim.arg(3)); sim.memo[bank + ".wstrb"] = random() & 15;
    for (unsigned i = 0; i < 2; ++i) {
      auto read = bank + ".read[" + std::to_string(i) + "]";
      auto write = bank + ".write[" + std::to_string(i) + "]";
      sim.memo[read + ".ready"] = random() & 1;
      sim.memo[write + ".valid"] = i == 0 ? writePulse : writeInit;
      sim.memo[write + ".bits"] = i == 0 ? pulseData : initData;
      require(sim.eval(sim.drivers.at(read + ".valid")) == 1 &&
              sim.eval(sim.drivers.at(write + ".ready")) == 1, "decoded bank stalled");
      require(sim.eval(sim.drivers.at(read + ".bits")) == (i == 0 ? remaining : initialized),
              "register readback differs from pre-edge state");
    }
    require(sim.eval(tokenValid) == initialized, "doneInit does not gate token valid");
    require(sim.eval(tokenBits) == ((remaining == 0) ^ activeHigh), "reset pulse polarity mismatch");
    bool fire = ready && initialized;
    writesDuringFire += writePulse && fire; stalls += !ready && initialized;
    accepted += fire && remaining != 0; zeroTokens += fire && remaining == 0;
    if (cycle >= 2 && cycle < 3000) directedPulseTokens += fire && remaining != 0;
    sim.edge();
    remaining = writePulse ? pulseData & mask : remaining - unsigned(fire && remaining != 0);
    initialized = !reset && (writeInit ? bool(initData & 1) : initialized);
  }
  require(writesDuringFire && stalls && accepted && zeroTokens, "pulse behavior coverage missing");
  require(directedPulseTokens == maximum, "programmed length differs from accepted pulse-token count");
}
void mapping(MLIRContext &context, bool first) {
  auto root = fixture(context, true, 1023, 0, first);
  auto c = *root->getOps<CircuitOp>().begin();
  auto original = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto ctor = cast<DictionaryAttr>(original[2]).get("widgetConstructorKey");
  auto info = cast<DictionaryAttr>(original[0]).get("channelInfo");
  std::string error;
  require(succeeded(goldengate::addResetPulseBridge(c, error)), error);
  require(succeeded(verify(*root)), "mapped reset bridge invalid");
  auto outer = named(c, "GGResetPulseBridgeWrapper"), inner = named(c, "GGClockBridgeControlWrapper");
  auto producer = named(c, "GGResetPulseBridge");
  require(outer.getPorts().size() == 4 && inner.getPorts().size() == 4 &&
          outer.getPortName(3) == "resetBridge_mcr" && outer.getPortDirection(3) == Direction::Out,
          "reset token boundary not replaced by decoded bank");
  require(producer->getAttr("goldengate.bridgeConstructor") == ctor, "constructor changed");
  auto registers = producer->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(registers.size() == 2, "missing register metadata");
  for (auto [i, attr] : llvm::enumerate(registers)) {
    auto reg = cast<DictionaryAttr>(attr);
    require(reg.getAs<IntegerAttr>("offset").getInt() == i * 4 &&
            reg.getAs<BoolAttr>("readable").getValue() && reg.getAs<BoolAttr>("writeable").getValue(),
            "genWOReg ReadWrite semantics lost");
  }
  auto all = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto ch = cast<DictionaryAttr>(all[0]);
  require(ch.get("channelInfo") == info && ch.getAs<ArrayAttr>("sources").size() == 1, "channel class lost");
  auto source = cast<StringAttr>(ch.getAs<ArrayAttr>("sources")[0]);
  require(source.getValue() == "~GGResetPulseBridgeWrapper|GGResetPulseBridge>hPort.reset.bits", "wrong token source");
  require(cast<StringAttr>(ch.getAs<ArrayAttr>("sinks")[0]).getValue() ==
          "~GGResetPulseBridgeWrapper|GGClockBridgeControlWrapper>resetTokens.bits", "sink must remain on inner wrapper");
  require(ch.getAs<StringAttr>("clock").getValue() ==
          "~GGResetPulseBridgeWrapper|GGResetPulseBridgeWrapper>hostClock", "copied clock target not transferred");
  require(cast<DictionaryAttr>(all[1]).getAs<StringAttr>("target").getValue() ==
          "~GGResetPulseBridgeWrapper|GGResetPulseBridgeWrapper>data", "copied port not transferred");
  require(cast<DictionaryAttr>(all[2]).get("widgetConstructorKey") == ctor, "annotation constructor changed");
  require(cast<DictionaryAttr>(all[2]).getAs<StringAttr>("target").getValue() ==
          "~GGResetPulseBridgeWrapper|GGResetPulseBridge>hPort", "bridge identity not transferred");
  require(bool(goldengate::resolveAnnotationTarget(c, source.getValue(), error)), "new source cannot resolve");
  InstanceOp sim, bridge;
  for (auto i : outer.getOps<InstanceOp>()) (i.getModuleName() == inner.getName() ? sim : bridge) = i;
  require(sim && bridge, "missing wrapper instances");
  unsigned host = 0, tokens = 0, mcr = 0;
  for (auto x : outer.getOps<StrictConnectOp>()) {
    auto port = cast<OpResult>(x.getDest()).getResultNumber();
    require(x.getDest().getDefiningOp() == bridge && port < 2 &&
            x.getSrc() == outer.getBodyBlock()->getArgument(port), "wrong host connection");
    ++host;
  }
  for (auto x : outer.getOps<ConnectOp>()) {
    if (x.getDest() == sim.getResult(first ? 0 : 2)) {
      auto sub = x.getSrc().getDefiningOp<SubfieldOp>();
      tokens += sub && sub.getFieldName() == "reset" && sub.getInput() == bridge.getResult(2);
    }
    mcr += x.getDest() == outer.getBodyBlock()->getArgument(3) && x.getSrc() == bridge.getResult(3);
  }
  require(host == 2 && tokens == 1 && mcr == 1, "missing clock/reset/token/bank wiring");
}
} // namespace
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    for (auto max : {1u, 5u, 1023u}) for (bool polarity : {false, true}) behavior(context, polarity, max);
    mapping(context, false); mapping(context, true);
    for (unsigned bad = 1; bad <= 11; ++bad) {
      auto root = fixture(context, true, 1023, bad); auto c = *root->getOps<CircuitOp>().begin();
      std::string before, after, error;
      llvm::raw_string_ostream a(before); root->print(a); a.flush();
      require(failed(goldengate::addResetPulseBridge(c, error)) && !error.empty(), "invalid input accepted");
      llvm::raw_string_ostream b(after); root->print(b); b.flush();
      require(before == after, "rejected input partially mutated IR");
    }
    llvm::outs() << "ResetPulseBridge: 120000 cycles, both polarities, readback, stalls, zero saturation, write/reset priority, target transfer and 11 atomic rejection cases passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
