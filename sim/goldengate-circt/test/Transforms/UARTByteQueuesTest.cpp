// See LICENSE for license details.
#include "goldengate/UARTSerialEngine.h"
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
// Interpret the generated operations, including actual MemOp read/write ports.
// A separate deque below checks ordering/occupancy without using pointer logic.
struct Interpreter {
  FModuleOp module;
  MemOp ram;
  std::array<uint8_t, 128> memory;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 128 && ram.getDataType() == UIntType::get(m.getContext(), 8, false) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    for (unsigned i = 0; i < memory.size(); ++i) memory[i] = i ^ 0xA5;
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
    firrtl.circuit "GGUARTSerialWrapper" {
      firrtl.module @GGUARTSerialWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %uart_tx: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        in %uart_rx: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %uart_fifoReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"uart_tx.bits", "other", "uart_fifoReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGUARTSerialWrapper|GGUARTSerialWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addUARTByteQueues(c, error)), error);
  require(succeeded(verify(*root)), "queue IR verification failed");
  Interpreter sim(named(c, "GGUARTByteQueue128")); std::deque<uint8_t> expected;
  std::mt19937 random(152);
  unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0;
  auto cycle = [&](bool reset, bool valid, bool ready, uint8_t data) {
    sim.memo.clear();
    sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = valid;
    sim.memo[sim.key(sim.arg(2)) + ".bits"] = data;
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = ready;
    bool enqReady = sim.drive(sim.arg(2), "ready"), deqValid = sim.drive(sim.arg(3), "valid");
    require(enqReady == (expected.size() < 128) && deqValid == !expected.empty(), "queue occupancy differs");
    if (deqValid) require(sim.drive(sim.arg(3), "bits") == expected.front(), "queue byte ordering differs");
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
    for (unsigned i = 0; i < 140; ++i) cycle(false, true, false, random());
    cycle(false, true, true, random()); // full cannot push on the same-cycle pop
    for (unsigned i = 0; i < 140; ++i) cycle(false, false, true, 0);
    cycle(false, true, true, random()); // empty cannot bypass the new byte
  }
  cycle(true, true, true, 0xE7);
  for (unsigned i = 0; i < 20000; ++i) {
    bool fill = i % 1500 < 750;
    cycle(i % 997 == 0, fill || bool(random() & 1), !fill || bool(random() & 1), random());
  }
  require(pushes > 1000 && pops > 1000 && fullBlocked && emptyBlocked && resetWrites && simultaneous,
          "queue boundary/reset/wrap coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addUARTByteQueues(c, error)), error);
  auto top = named(c, "GGUARTQueueWrapper");
  require(top.getNumPorts() == 5 && top.getPortName(4) == "other", "FIFO reset remained external or copied port lost");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGUARTQueueWrapper|GGUARTQueueWrapper>uart_tx.bits",
      "~GGUARTQueueWrapper|GGUARTQueueWrapper>other", "~GGUARTQueueWrapper|GGUARTSerialWrapper>uart_fifoReset"};
  require(annos.size() == 3, "annotations lost");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == targets[i], "annotation target differs");
  std::map<std::string, InstanceOp> instances;
  for (auto inst : top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  require(instances.size() == 3, "wrapper needs sim and two queues");
  auto sim = instances.at("sim"), tx = instances.at("txfifo"), rx = instances.at("rxfifo");
  require(tx.getModuleName() == "GGUARTByteQueue128" && rx.getModuleName() == tx.getModuleName(), "wrong queue instance");
  auto bulk = [&](Value dest, Value src) {
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  auto wire = [&](Value dest, Value src) {
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  require(bulk(tx.getResult(2), sim.getResult(2)) && bulk(top.getBodyBlock()->getArgument(2), tx.getResult(3)) &&
      bulk(rx.getResult(2), top.getBodyBlock()->getArgument(3)) && bulk(sim.getResult(3), rx.getResult(3)), "TX/RX queue direction differs");
  for (auto fifo : {tx, rx}) require(wire(fifo.getResult(0), top.getBodyBlock()->getArgument(0)) &&
      wire(fifo.getResult(1), sim.getResult(4)), "queue clock/accepted-target-reset wiring differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 5; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    auto top = named(c, "GGUARTSerialWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[4] = b.getStringAttr("WrongReset"); top.setPortNames(names);
    }
    if (bad == 3) {
      b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used");
    }
    if (bad == 4) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGUARTByteQueue128"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addUARTByteQueues(c, error)), "invalid byte queue boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); rejection(context);
    llvm::outs() << "UART byte queues: ordering, full/empty stalls, wrap, reset/write, wiring and atomic rejection passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
