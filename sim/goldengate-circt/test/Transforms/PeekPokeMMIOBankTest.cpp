// See LICENSE for license details.
#include "goldengate/PeekPokeCycleEngine.h"
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
#include <deque>
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
struct BankInterpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  BankInterpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = cast<UIntType>(r.getResult().getType()).getWidthOrSentinel() == 64 ? 0xFEDCBA9876543210ULL : 1; }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 2 && reset == 2, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
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
// Interpret the generated operations, including actual MemOp read/write ports.
// A separate deque below checks ordering/occupancy without using pointer logic.
struct QueueInterpreter {
  FModuleOp module;
  MemOp ram;
  std::array<uint32_t, 2> memory;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  QueueInterpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 2 && ram.getDataType() == UIntType::get(m.getContext(), 32, false) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    for (unsigned i = 0; i < memory.size(); ++i) memory[i] = i ^ 0xA5A5A5A5;
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput() == ram.getResult(0) && f.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en") == 1, "queue read disabled");
      n = memory.at(drive(ram.getResult(0), "addr"));
    } else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    Value writer = ram.getResult(1);
    if (drive(writer, "en") && drive(writer, "mask"))
      memory.at(drive(writer, "addr")) = drive(writer, "data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGPeekPokeCycleWrapper" {
      firrtl.module @GGPeekPokeCycleWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %peekPokeBridge_cycle: !firrtl.bundle<step flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, poke flip: uint<1>, resetValue flip: uint<1>, tCycle: uint<64>, done: uint<1>, precisePeekable: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"peekPokeBridge_cycle.resetValue", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGPeekPokeCycleWrapper|GGPeekPokeCycleWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void bankBehavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addPeekPokeMMIOBank(c, error)), error);
  require(succeeded(verify(*root)), "PeekPoke bank IR verification failed");
  BankInterpreter sim(named(c, "GGPeekPokeMMIOBank"));
  uint64_t snapshot = 0xFEDCBA9876543210ULL;
  uint32_t done = 1, precise = 1; bool resetValue = true;
  std::mt19937_64 random(155);
  unsigned overrides = 0, resetWrites = 0, disabledLatch = 0;
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    bool reset = cycle & 1, doneSample = cycle & 2, preciseSample = cycle & 4;
    unsigned writeMask = (cycle >> 3) & 127, readMask = random() & 127;
    uint64_t currentCycle = random();
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".tCycle"] = currentCycle;
    sim.memo[sim.key(sim.arg(2)) + ".done"] = doneSample;
    sim.memo[sim.key(sim.arg(2)) + ".precisePeekable"] = preciseSample;
    std::array<uint32_t, 7> data;
    std::array<uint32_t, 7> reads{uint32_t(snapshot), uint32_t(snapshot >> 32), 0, 0, done, unsigned(resetValue), precise};
    for (unsigned i = 0; i < 7; ++i) {
      std::string lane = sim.key(sim.arg(3)) + ".write[" + std::to_string(i) + "]";
      sim.memo[lane + ".valid"] = bool(writeMask & (1 << i));
      data[i] = random(); sim.memo[lane + ".bits"] = data[i];
      sim.memo[sim.key(sim.arg(3)) + ".read[" + std::to_string(i) + "].ready"] = bool(readMask & (1 << i));
      require(sim.output(3, "read[" + std::to_string(i) + "].bits") == reads[i], "bank read must see pre-edge state");
      require(sim.output(3, "read[" + std::to_string(i) + "].valid") == unsigned(i != 3), "STEP read-valid differs");
      if (i != 3) require(sim.output(3, "write[" + std::to_string(i) + "].ready") == 1, "register write stalled");
    }
    require(sim.output(2, "poke") == bool(writeMask & 32) && sim.output(2, "resetValue") == resetValue,
        "poke must expose write-valid and pre-edge unreset data");
    unsigned assertions = 0;
    for (auto assertion : sim.module.getOps<AssertOp>()) {
      bool predicate = sim.eval(assertion->getOperand(1)), enabled = sim.eval(assertion->getOperand(2));
      unsigned i = assertions++;
      bool forbidden = i < 2 ? bool(writeMask & (1 << i)) : bool(readMask & (1 << i));
      require(predicate == !forbidden && enabled == !reset, "access assertion/reset gate differs");
    }
    require(assertions == 4, "access assertions missing");
    if ((writeMask & 4) && (data[2] & 1)) snapshot = currentCycle;
    disabledLatch += (writeMask & 4) && !(data[2] & 1);
    if (writeMask & 32) resetValue = data[5] & 1;
    done = reset ? 0 : (writeMask & 16) ? data[4] : unsigned(doneSample);
    precise = reset ? 0 : (writeMask & 64) ? data[6] : unsigned(preciseSample);
    overrides += !reset && (writeMask & 80); resetWrites += reset && (writeMask & 36);
    sim.edge(); sim.memo.clear();
    require(sim.output(3, "read[0].bits") == uint32_t(snapshot) &&
        sim.output(3, "read[1].bits") == uint32_t(snapshot >> 32) &&
        sim.output(3, "read[4].bits") == done && sim.output(3, "read[5].bits") == resetValue &&
        sim.output(3, "read[6].bits") == precise, "latch/sample/write/reset precedence differs");
  }
  require(overrides && resetWrites && disabledLatch, "MMIO corner coverage missing");
}
void queueBehavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addPeekPokeMMIOBank(c, error)), error);
  require(succeeded(verify(*root)), "queue IR verification failed");
  QueueInterpreter sim(named(c, "GGPeekPokeStepQueue")); std::deque<uint32_t> expected;
  std::mt19937 random(155);
  unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0;
  auto cycle = [&](bool reset, bool valid, bool ready, uint32_t data) {
    sim.memo.clear();
    sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = valid;
    sim.memo[sim.key(sim.arg(2)) + ".bits"] = data;
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = ready;
    bool enqReady = sim.drive(sim.arg(2), "ready"), deqValid = sim.drive(sim.arg(3), "valid");
    require(enqReady == (expected.size() < 2) && deqValid == !expected.empty(), "queue occupancy differs");
    if (deqValid) require(sim.drive(sim.arg(3), "bits") == expected.front(), "queue word ordering differs");
    bool push = valid && enqReady, pop = ready && deqValid;
    fullBlocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    simultaneous += push && pop; pushes += push; pops += pop; resetWrites += reset && push;
    auto oldMemory = sim.memory;
    unsigned address = sim.drive(sim.ram.getResult(1), "addr");
    if (pop) expected.pop_front(); if (push) expected.push_back(data);
    if (reset) expected.clear();
    if (push) oldMemory.at(address) = data;
    sim.edge(); require(sim.memory == oldMemory, "reset incorrectly gates writes or clears RAM");
    sim.memo.clear();
    // The async read sees the new memory and pointer after the same edge.
    require(sim.drive(sim.arg(3), "bits") == sim.memory.at(sim.drive(sim.ram.getResult(0), "addr")), "async read differs");
  };
  cycle(true, false, false, 0);
  // Fill beyond full and drain beyond empty, with both handshakes at boundaries.
  for (unsigned round = 0; round < 8; ++round) {
    for (unsigned i = 0; i < 4; ++i) cycle(false, true, false, random());
    cycle(false, true, true, random()); // full cannot push on the same-cycle pop
    for (unsigned i = 0; i < 4; ++i) cycle(false, false, true, 0);
    cycle(false, true, true, random()); // empty cannot bypass the new word
  }
  cycle(true, true, true, 0xDEADBEEF);
  for (unsigned i = 0; i < 20000; ++i) {
    bool fill = i % 1500 < 750;
    cycle(i % 997 == 0, fill || bool(random() & 1), !fill || bool(random() & 1), random());
  }
  require(pushes > 1000 && pops > 1000 && fullBlocked && emptyBlocked && resetWrites && simultaneous,
          "queue boundary/reset/wrap coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addPeekPokeMMIOBank(c, error)), error);
  auto top = named(c, "GGPeekPokeMMIOWrapper"), bank = named(c, "GGPeekPokeMMIOBank");
  require(top.getNumPorts() == 4 && top.getPortName(2) == "other" && top.getPortName(3) == "peekPokeBridge_mcr", "cycle port remained external");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const llvm::StringRef names[]{"tCycle_0", "tCycle_1", "tCycle_latch", "STEP", "DONE", "reset_0", "PRECISE_PEEKABLE"};
  require(regs && regs.size() == 7, "register map missing");
  for (unsigned i = 0; i < 7; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name") == names[i] && reg.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
        reg.getAs<BoolAttr>("readable").getValue() == (i != 2 && i != 3) &&
        reg.getAs<BoolAttr>("writeable").getValue() == (i >= 2), "register permissions/offsets differ");
  }
  auto bulk = [&](FModuleOp m, Value dest, Value src) {
    for (auto conn : m.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  std::map<std::string, InstanceOp> instances;
  for (auto inst : top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  auto sim = instances.at("sim"), mmio = instances.at("peekPokeRegisters");
  require(bulk(top, mmio.getResult(2), sim.getResult(2)) &&
      bulk(top, top.getBodyBlock()->getArgument(3), mmio.getResult(3)), "cycle/MCR wrapper wiring differs");
  for (unsigned i = 0; i < 2; ++i) {
    bool connected = false;
    for (auto conn : top.getOps<StrictConnectOp>()) connected |= conn.getDest() == mmio.getResult(i) && conn.getSrc() == top.getBodyBlock()->getArgument(i);
    require(connected, "bank must use host clock/reset");
  }
  auto queue = *bank.getOps<InstanceOp>().begin();
  require(queue.getModuleName() == "GGPeekPokeStepQueue", "STEP queue missing");
  unsigned queueBulk = 0;
  for (auto conn : bank.getOps<ConnectOp>()) {
    if (conn.getDest() == queue.getResult(2)) {
      auto lane = conn.getSrc().getDefiningOp<SubindexOp>();
      require(lane && lane.getIndex() == 3 && lane.getInput().getDefiningOp<SubfieldOp>().getFieldName() == "write", "STEP must use word three");
      ++queueBulk;
    }
    if (conn.getSrc() == queue.getResult(3)) {
      auto field = conn.getDest().getDefiningOp<SubfieldOp>();
      require(field && field.getFieldName() == "step" && field.getInput() == bank.getBodyBlock()->getArgument(2), "STEP consumer direction differs");
      ++queueBulk;
    }
  }
  require(queueBulk == 2, "STEP bulk handshake wiring missing");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGPeekPokeMMIOWrapper|GGPeekPokeCycleWrapper>peekPokeBridge_cycle.resetValue",
      "~GGPeekPokeMMIOWrapper|GGPeekPokeMMIOWrapper>other", "~GGPeekPokeMMIOWrapper|GGPeekPokeMMIOWrapper>hostReset"};
  require(annos.size() == 3, "annotation lost");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == targets[i], "annotation target differs");
  require(succeeded(goldengate::mapPeekPokeBridgeControl(c, 25, 12, error)), error);
  require(succeeded(verify(*root)), "bank and transport composition failed verification");
  auto control = named(c, "GGPeekPokeBridgeControlWrapper");
  require(control.getNumPorts() == 4 && control.getPortName(2) == "other" &&
      control.getPortName(3) == "peekPokeBridge_ctrl", "decoded bank remained external");
  instances.clear();
  for (auto inst : control.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  auto inner = instances.at("sim"), crFile = instances.at("crFile");
  require(crFile.getModuleName() == "GGPeekPokeMCRFile" &&
      bulk(control, crFile.getResult(2), control.getBodyBlock()->getArgument(3)) &&
      bulk(control, crFile.getResult(3), inner.getResult(3)), "Nasti/MCR transport wiring differs");
  for (unsigned i = 0; i < 2; ++i) {
    bool connected = false;
    for (auto conn : control.getOps<StrictConnectOp>())
      connected |= conn.getDest() == crFile.getResult(i) && conn.getSrc() == control.getBodyBlock()->getArgument(i);
    require(connected, "transport must use host clock/reset");
  }
  annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef controlTargets[]{
      "~GGPeekPokeBridgeControlWrapper|GGPeekPokeCycleWrapper>peekPokeBridge_cycle.resetValue",
      "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>other",
      "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>hostReset"};
  require(annos.size() == 3, "transport lost annotation");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == controlTargets[i], "transport annotation target differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGPeekPokeCycleWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 5) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 1 : 3] = b.getStringAttr(bad == 2 ? "WrongReset" : "peekPokeBridge_mcr"); top.setPortNames(names);
    }
    if (bad == 3) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 4) {
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGPeekPokeMMIOBank"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addPeekPokeMMIOBank(c, error)), "invalid PeekPoke MMIO boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    bankBehavior(context); queueBehavior(context); mapping(context); rejection(context);
    llvm::outs() << "PeekPoke MMIO: 32768 bank cycles, STEP FIFO ordering/stalls/reset, permissions, target identity and atomic rejection passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
