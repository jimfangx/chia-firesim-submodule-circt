// See LICENSE for license details.
#include "goldengate/UARTSerialEngine.h"
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
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<DShlPrimOp>(op)) n = eval(op->getOperand(0)) << eval(op->getOperand(1));
    else if (isa_and_nonnull<DShrPrimOp>(op)) n = eval(op->getOperand(0)) >> eval(op->getOperand(1));
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned div, unsigned bad = 0) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGResetPulseBridgeControlWrapper" {
      firrtl.module @GGResetPulseBridgeControlWrapper(
        out %resetTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %rxTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %txTokens: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context); NamedAttrList mapping;
  const llvm::StringRef locals[]{"reset", "uart_txd", "uart_rxd"};
  const llvm::StringRef ports[]{"resetTokens", "txTokens", "rxTokens"};
  SmallVector<Attribute> annos;
  for (unsigned j = 0; j < 3; ++j) {
    auto name = b.getStringAttr("ep_1_" + locals[j].str()); mapping.set(locals[j], name);
    NamedAttrList channel;
    channel.set("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection));
    channel.set("globalName", name);
    channel.set("channelInfo", b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
      b.getNamedAttr("latency", b.getI64IntegerAttr(bad == 3 ? 0 : 1))}));
    channel.set("clock", b.getStringAttr("~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>hostClock"));
    channel.set(j == 2 ? "sinks" : "sources", b.getArrayAttr({b.getStringAttr(
      "~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>" + ports[j].str() + (bad == 4 ? ".valid" : ".bits"))}));
    if (bad == 5) channel.set(j == 2 ? "sources" : "sinks", channel.get(j == 2 ? "sinks" : "sources"));
    annos.push_back(channel.getDictionary(&context));
  }
  auto bridge = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
      b.getNamedAttr("widgetClass", b.getStringAttr("firechip.goldengateimplementations.UARTBridgeModule")),
      b.getNamedAttr("target", b.getStringAttr("~Original|Original>ep_1")),
      b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("firechip.bridgeinterfaces.UARTKey")),
        b.getNamedAttr("div", b.getI64IntegerAttr(bad == 2 ? 0 : div))})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&context))});
  if (bad != 1) annos.push_back(bridge);
  if (bad == 6) annos.push_back(bridge);
  if (bad == 7) annos.push_back(annos[0]);
  annos.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DontTouch)),
    b.getNamedAttr("target", b.getStringAttr("~GGResetPulseBridgeControlWrapper|GGResetPulseBridgeControlWrapper>other"))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context, unsigned div) {
  auto root = fixture(context, div); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addUARTSerialEngine(c, error)), error);
  require(succeeded(verify(*root)), "UART IR verification failed");
  auto engine = named(c, "GGUARTSerialEngine"); Interpreter sim(engine);
  std::map<std::string, uint64_t> model{{"txState",0},{"txData",255},{"rxState",0},{"txDataIdx",0},
      {"txBaudCount",0},{"txSlackCount",0},{"rxBaudCount",0},{"rxDataIdx",0}};
  for (auto r : engine.getOps<RegOp>()) sim.state[r.getResult()] = model[r.getName().str()];
  auto k = [&](unsigned port, llvm::StringRef suffix) { return sim.key(sim.arg(port)) + suffix.str(); };
  std::mt19937 random(151); unsigned enqueue = 0, dequeue = 0, stall = 0, targetResetCount = 0, breakCount = 0;
  for (unsigned cycle = 0; cycle < 50000; ++cycle) {
    bool reset = cycle == 0 || cycle % 9973 == 0;
    bool hv = cycle < 25000 ? true : bool(random() & 1);
    bool hr = cycle < 25000 ? true : bool(random() & 1);
    bool ready = cycle < 25000 ? true : bool(random() & 1);
    bool txd = cycle < 12500 ? false : cycle < 25000 ? true : bool(random() & 1);
    bool rxvalid = cycle < 25000 || bool(random() & 1), targetReset = cycle % 173 == 0;
    unsigned rxbyte = random() & 255;
    sim.memo.clear(); sim.memo[k(0, "")] = 0; sim.memo[k(1, "")] = reset;
    sim.memo[k(2, ".toHost.hValid")] = hv; sim.memo[k(2, ".fromHost.hReady")] = hr;
    sim.memo[k(2, ".hBits.reset")] = targetReset; sim.memo[k(2, ".hBits.uart.txd")] = txd;
    sim.memo[k(3, ".ready")] = ready; sim.memo[k(4, ".valid")] = rxvalid; sim.memo[k(4, ".bits")] = rxbyte;
    bool fire = hv && hr && ready;
    auto out = [&](unsigned port, llvm::StringRef suffix) { return sim.eval(sim.drivers.at(k(port, suffix))); };
    unsigned ts = model["txState"], rs = model["rxState"];
    bool tw = ts == 2 && fire && model["txDataIdx"] == 7;
    bool tb = ts == 1 && fire && model["txBaudCount"] == div - 1;
    bool slack = ts == 0 && !txd && fire && model["txSlackCount"] == 3;
    bool rb = fire && model["rxBaudCount"] == div - 1;
    bool rw = rs == 2 && fire && rb && model["rxDataIdx"] == 7;
    unsigned rxd = rs == 1 ? 0 : rs == 2 ? (rxbyte >> model["rxDataIdx"]) & 1 : 1;
    require(out(2, ".toHost.hReady") == fire && out(2, ".fromHost.hValid") == fire, "UART token fire differs");
    require(out(2, ".hBits.uart.rxd") == rxd && out(4, ".ready") == rw, "UART RX output differs");
    require(out(3, ".valid") == tw && out(3, ".bits") == model["txData"], "UART TX old-data enqueue differs");
    require(out(5, "") == (reset || (fire && targetReset)), "UART FIFO reset differs");
    enqueue += tw; dequeue += rw; stall += !fire; targetResetCount += fire && targetReset; breakCount += ts == 3;
    auto next = model;
    auto advance = [&](const char *name, unsigned limit, bool enable) { if (enable) next[name] = model[name] == limit - 1 ? 0 : model[name] + 1; };
    advance("txDataIdx",8,ts == 2 && fire); advance("txBaudCount",div,ts == 1 && fire);
    advance("txSlackCount",4,ts == 0 && !txd && fire); advance("rxBaudCount",div,fire);
    advance("rxDataIdx",8,rs == 2 && fire && rb);
    if (slack) next["txData"] = 0; else if (ts == 2 && fire) next["txData"] |= unsigned(txd) << model["txDataIdx"];
    if (ts == 0 && slack) next["txState"] = 1;
    if (ts == 1 && tb) next["txState"] = 2;
    if (ts == 2 && fire) next["txState"] = tw ? txd ? 0 : 3 : 1;
    if (ts == 3 && txd && fire) next["txState"] = 0;
    if (rs == 0 && rb && rxvalid) next["rxState"] = 1;
    if (rs == 1 && rb) next["rxState"] = 2;
    if (rs == 2 && rw && rb) next["rxState"] = 0;
    if (reset) for (auto &[name, value] : next) if (name != "txData") value = 0;
    sim.edge();
    for (auto r : engine.getOps<RegResetOp>()) require(sim.state.lookup(r.getResult()) == next.at(r.getName().str()), "UART reset-register transition differs");
    for (auto r : engine.getOps<RegOp>()) require(sim.state.lookup(r.getResult()) == next.at(r.getName().str()), "UART unreset TX data transition differs");
    model = next;
  }
  require(enqueue && dequeue && stall && targetResetCount && breakCount, "UART behavior coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context,271); auto c = *root->getOps<CircuitOp>().begin();
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  require(succeeded(goldengate::addUARTSerialEngine(c,error)),error);
  require(succeeded(verify(*root)),"UART wrapper invalid");
  auto top = named(c,"GGUARTSerialWrapper");
  require(top.getNumPorts() == 6 && top.getPortName(3) == "uart_tx" && top.getPortName(4) == "uart_rx" && top.getPortName(5) == "uart_fifoReset", "UART serial ports not replaced");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == annos.size(),"UART annotation classes lost");
  for (unsigned j=0;j<3;++j) {
    auto before=cast<DictionaryAttr>(raw[j]), after=cast<DictionaryAttr>(annos[j]);
    require(before.get("channelInfo") == after.get("channelInfo"),"UART channel semantics changed");
    for (auto side : {"sources","sinks"}) for (auto attr : after.getAs<ArrayAttr>(side))
      require(bool(goldengate::resolveAnnotationTarget(c,cast<StringAttr>(attr).getValue(),error)),error);
  }
  require(cast<DictionaryAttr>(annos[3]).get("widgetConstructorKey") == cast<DictionaryAttr>(raw[3]).get("widgetConstructorKey"), "UART constructor changed");
  require(cast<DictionaryAttr>(annos[4]).getAs<StringAttr>("target") == "~GGUARTSerialWrapper|GGUARTSerialWrapper>other", "copied port annotation not retargeted");
}
void rejection(MLIRContext &context) {
  for (unsigned bad=1;bad<=7;++bad) {
    auto root=fixture(context,271,bad); std::string before,after,error;
    {llvm::raw_string_ostream s(before);root->print(s);}
    require(failed(goldengate::addUARTSerialEngine(*root->getOps<CircuitOp>().begin(),error)),"bad UART boundary accepted");
    {llvm::raw_string_ostream s(after);root->print(s);}
    require(before==after,"rejected UART mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    for (unsigned div : {1,2,3,271}) behavior(context,div);
    mapping(context);rejection(context);
    llvm::outs()<<"UART serial engine: 200000 cycles, mapping and atomic rejection passed\n";return 0;
  } catch (const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
