// See LICENSE for license details.
#include "goldengate/UARTSerialEngine.h"
#include "goldengate/ClockBridgeControl.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
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
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5 & ((1 << cast<UIntType>(r.getResult().getType()).getWidthOrSentinel()) - 1); }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 4 && reset == 2, "wrong bank reset policy");
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
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGUARTQueueWrapper" {
      firrtl.module @GGUARTQueueWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %uart_tx: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        in %uart_rx: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"uart_tx.bits", "uart_rx.valid", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGUARTQueueWrapper|GGUARTQueueWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addUARTMMIOBank(c, error)), error);
  require(succeeded(verify(*root)), "bank IR verification failed");
  Interpreter sim(named(c, "GGUARTMMIOBank"));
  std::array<unsigned, 6> expected{0xA5, 1, 1, 0xA5, 1, 1};
  std::mt19937 random(153);
  unsigned writeDuringReset = 0, consecutivePulse = 0, statusOverride = 0;
  bool previousPulse = false;
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    // Exhaust every reset/write-mask/status-valid/status-ready combination,
    // including illegal simultaneous lane writes to verify independent words.
    bool reset = cycle & 1, txValid = cycle & 2, rxReady = cycle & 4;
    unsigned writeMask = (cycle >> 3) & 63, txBits = random() & 255;
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".bits"] = txBits;
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = txValid;
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = rxReady;
    std::array<uint32_t, 6> data;
    for (unsigned i = 0; i < 6; ++i) {
      std::string lane = sim.key(sim.arg(4)) + ".write[" + std::to_string(i) + "]";
      sim.memo[lane + ".valid"] = bool(writeMask & (1 << i));
      data[i] = random(); sim.memo[lane + ".bits"] = data[i];
      require(sim.output(4, "read[" + std::to_string(i) + "].bits") == expected[i], "read must see pre-edge register");
      require(sim.output(4, "read[" + std::to_string(i) + "].valid") == 1 &&
          sim.output(4, "write[" + std::to_string(i) + "].ready") == 1, "MCR register handshake differs");
    }
    require(sim.output(2, "ready") == expected[2] && sim.output(3, "bits") == expected[3] &&
        sim.output(3, "valid") == expected[4], "queue sees current MMIO register");
    std::array<unsigned, 6> next{txBits, unsigned(txValid), 0, expected[3], 0, unsigned(rxReady)};
    for (unsigned i = 0; i < 6; ++i) if (writeMask & (1 << i)) next[i] = data[i] & ((i == 0 || i == 3) ? 255 : 1);
    writeDuringReset += reset && writeMask; statusOverride += (writeMask & 35) != 0;
    if (reset) next[2] = next[4] = 0;
    bool pulse = next[2] || next[4]; consecutivePulse += pulse && previousPulse; previousPulse = pulse;
    sim.edge(); sim.memo.clear(); expected = next;
    for (unsigned i = 0; i < 6; ++i)
      require(sim.output(4, "read[" + std::to_string(i) + "].bits") == expected[i], "write/sample/reset precedence differs");
  }
  // The alternating reset above intentionally prevents consecutive pulses.
  // Explicitly verify back-to-back writes extend pulses, then clear next edge.
  for (unsigned cycle = 0; cycle < 4; ++cycle) {
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = 0;
    sim.memo[sim.key(sim.arg(2)) + ".bits"] = 0; sim.memo[sim.key(sim.arg(2)) + ".valid"] = 0;
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = 0;
    for (unsigned i = 0; i < 6; ++i) {
      std::string lane = sim.key(sim.arg(4)) + ".write[" + std::to_string(i) + "]";
      sim.memo[lane + ".valid"] = cycle < 3 && (i == 2 || i == 4); sim.memo[lane + ".bits"] = 1;
    }
    sim.edge(); sim.memo.clear();
    require(sim.output(2, "ready") == unsigned(cycle < 3) && sim.output(3, "valid") == unsigned(cycle < 3), "pulse retrigger/clear differs");
    consecutivePulse += cycle == 1;
  }
  require(writeDuringReset && consecutivePulse && statusOverride, "bank coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addUARTMMIOBank(c, error)), error);
  auto top = named(c, "GGUARTMMIOWrapper"); auto bank = named(c, "GGUARTMMIOBank");
  require(top.getNumPorts() == 4 && top.getPortName(3) == "uartBridge_mcr", "byte ports remained external");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const llvm::StringRef names[]{"out_bits", "out_valid", "out_ready", "in_bits", "in_valid", "in_ready"};
  require(regs && regs.size() == 6, "register map missing");
  for (unsigned i = 0; i < 6; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name") == names[i] && reg.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
        reg.getAs<BoolAttr>("readable").getValue() && reg.getAs<BoolAttr>("writeable").getValue(), "register map differs");
  }
  std::map<std::string, InstanceOp> instances;
  for (auto inst : top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  auto sim = instances.at("sim"), mmio = instances.at("uartRegisters");
  auto bulk = [&](Value dest, Value src) {
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  require(bulk(mmio.getResult(2), sim.getResult(2)) && bulk(sim.getResult(3), mmio.getResult(3)) &&
      bulk(top.getBodyBlock()->getArgument(3), mmio.getResult(4)), "UART queue/MCR wiring differs");
  for (unsigned i = 0; i < 2; ++i) {
    bool connected = false;
    for (auto conn : top.getOps<StrictConnectOp>()) connected |= conn.getDest() == mmio.getResult(i) && conn.getSrc() == top.getBodyBlock()->getArgument(i);
    require(connected, "bank must use host clock/reset");
  }
  require(succeeded(goldengate::mapUARTBridgeControl(c, 25, 12, error)), error);
  require(succeeded(verify(*root)), "UART control IR verification failed");
  top = named(c, "GGUARTBridgeControlWrapper");
  require(top.getPortName(3) == "uartBridge_ctrl" && top.getPortDirection(3) == Direction::In, "UART Nasti boundary missing");
  auto adapter = named(c, "GGUARTMCRFile");
  require(cast<FVectorType>(cast<BundleType>(adapter.getPortType(3)).getElement("read")->type).getNumElements() == 6, "wrong UART bank size");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGUARTBridgeControlWrapper|GGUARTQueueWrapper>uart_tx.bits",
      "~GGUARTBridgeControlWrapper|GGUARTQueueWrapper>uart_rx.valid", "~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>other",
      "~GGUARTBridgeControlWrapper|GGUARTBridgeControlWrapper>hostReset"};
  for (unsigned i = 0; i < 4; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == targets[i], "copied/internal annotation identity differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGUARTQueueWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 5) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 1 : 4] = b.getStringAttr(bad == 2 ? "WrongReset" : "uartBridge_mcr"); top.setPortNames(names);
    }
    if (bad == 3) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 4) {
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGUARTMMIOBank"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addUARTMMIOBank(c, error)), "invalid UART MMIO boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); rejection(context);
    llvm::outs() << "UART MMIO: samples, write priority/truncation, pulses, reset, queue/control wiring, register map and atomic rejection passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
