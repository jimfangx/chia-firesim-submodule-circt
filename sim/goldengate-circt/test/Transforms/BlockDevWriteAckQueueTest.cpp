// See LICENSE for license details.
#include "goldengate/BlockDevWriteAckQueue.h"
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
// Interpret the generated FIRRTL, including the real RAM ports. The deque
// reference tracks data ordering independently of the hardware pointers.
struct Interpreter {
  FModuleOp module;
  MemOp ram;
  std::array<APInt, 4> memory;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  unsigned width(Value v) { return cast<UIntType>(v.getType()).getWidthOrSentinel(); }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 4 && ram.getDataType() == UIntType::get(m.getContext(), 1, false) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    for (unsigned i = 0; i < memory.size(); ++i) memory[i] = APInt(1, i & 1);
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  APInt drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  APInt eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); APInt n(width(v), 0);
    if (isa_and_nonnull<RegResetOp>(op)) {
      if (state.count(v)) n = state.lookup(v);
    } else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zext(width(v)) + eval(op->getOperand(1)).zext(width(v));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(!eval(op->getOperand(0)).isZero() ? 1 : 2));
    else if (auto cat = dyn_cast_or_null<CatPrimOp>(op)) {
      auto a = eval(op->getOperand(0)), b = eval(op->getOperand(1));
      n = (a.zext(width(v)) << b.getBitWidth()) | b.zext(width(v));
    } else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(bits.getInput()).lshr(bits.getLo()).trunc(width(v));
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput() == ram.getResult(0) && f.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en").getZExtValue() == 1, "queue read disabled");
      n = memory.at(drive(ram.getResult(0), "addr").getZExtValue());
    } else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(width(v)); memo.insert_or_assign(k, n); return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = !eval(r.getResetSignal()).isZero() ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    Value writer = ram.getResult(1);
    if (!drive(writer, "en").isZero() && !drive(writer, "mask").isZero())
      memory.at(drive(writer, "addr").getZExtValue()) = drive(writer, "data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGBlockDevReadResponseQueueWrapper" {
      firrtl.module @GGBlockDevReadResponseQueueWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %blockdev_wack_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        out %blockdev_queue_reset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"blockdev_wack_deq.bits", "other", "blockdev_queue_reset", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGBlockDevReadResponseQueueWrapper|GGBlockDevReadResponseQueueWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos));
  return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevWriteAckQueue(c, error)), error);
  require(succeeded(verify(*root)), "queue IR verification failed");
  Interpreter sim(named(c, "GGBlockDevWriteAckQueue4")); std::deque<APInt> expected;
  std::mt19937_64 random(199);
  unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0, wraps = 0, cycles = 0;
  auto beat = [&]() { return bool(random() & 1); };
  auto cycle = [&](bool reset, bool valid, bool ready, bool data) {
    ++cycles; sim.memo.clear();
    auto input = [&](Value root, llvm::StringRef field, unsigned width, uint64_t value) { sim.memo.insert_or_assign(sim.key(root) + "." + field.str(), APInt(width, value)); };
    sim.memo.insert_or_assign(sim.key(sim.arg(1)), APInt(1, reset));
    input(sim.arg(2), "valid", 1, valid); input(sim.arg(2), "bits", 1, data);
    input(sim.arg(3), "ready", 1, ready);
    bool enqReady = !sim.drive(sim.arg(2), "ready").isZero(), deqValid = !sim.drive(sim.arg(3), "valid").isZero();
    require(enqReady == (expected.size() < 4) && deqValid == !expected.empty(), "queue occupancy differs");
    auto output = [&]() { return sim.drive(sim.arg(3), "bits"); };
    if (deqValid) require(output() == expected.front(), "write acknowledgment tag order differs");
    unsigned readAddress = sim.drive(sim.ram.getResult(0), "addr").getZExtValue();
    require(readAddress < 4 && output() == sim.memory.at(readAddress), "async read differs even when invalid");
    bool push = valid && enqReady, pop = ready && deqValid;
    fullBlocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    simultaneous += push && pop; pushes += push; pops += pop; resetWrites += reset && push;
    auto oldMemory = sim.memory;
    unsigned address = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    require(address < 4, "enqueue pointer escaped queue depth");
    if (pop) expected.pop_front(); if (push) expected.push_back(APInt(1, data)); if (reset) expected.clear();
    if (push) oldMemory.at(address) = APInt(1, data);
    sim.edge(); require(sim.memory == oldMemory, "reset clears RAM or suppresses accepted write"); sim.memo.clear();
    unsigned nextAddress = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    if (!reset && push && address == 3) { require(nextAddress == 0, "modulo-4 pointer wrap differs"); ++wraps; }
    require(output() == sim.memory.at(sim.drive(sim.ram.getResult(0), "addr").getZExtValue()), "post-edge asynchronous output differs");
  };
  cycle(true, false, false, beat());
  for (unsigned round = 0; round < 12; ++round) {
    for (unsigned i = 0; i < 9; ++i) cycle(false, true, false, beat());
    cycle(false, true, true, beat()); // Full: accepted pop cannot enable same-cycle push.
    for (unsigned i = 0; i < 9; ++i) cycle(false, false, true, beat());
    cycle(false, true, true, beat()); // Empty: enqueued word cannot flow through.
  }
  cycle(true, true, true, true);
  for (unsigned i = 0; i < 20000; ++i) {
    bool fill = i % 1500 < 750;
    cycle(i % 997 == 0, fill || bool(random() & 1), !fill || bool(random() & 1), beat());
  }
  require(pushes > 1000 && pops > 1000 && fullBlocked && emptyBlocked && resetWrites && simultaneous && wraps > 30,
          "queue boundary/reset/modulo-4 wrap coverage missing");
  llvm::outs() << "BlockDev write-ack queue: " << cycles << " cycles, " << wraps << " wrap events passed\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevWriteAckQueue(c, error)), error);
  require(succeeded(verify(*root)), "write-ack wrapper failed IR verification");
  auto top = named(c, "GGBlockDevWriteAckQueueWrapper");
  require(top.getNumPorts() == 5 && top.getPortName(2) == "blockdev_queue_reset" &&
      top.getPortName(3) == "other" && top.getPortName(4) == "blockdev_wack_enq" &&
      top.getPortDirection(4) == Direction::In, "copied ports or acknowledgment enqueue boundary differ");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{
      "~GGBlockDevWriteAckQueueWrapper|GGBlockDevReadResponseQueueWrapper>blockdev_wack_deq.bits",
      "~GGBlockDevWriteAckQueueWrapper|GGBlockDevWriteAckQueueWrapper>other",
      "~GGBlockDevWriteAckQueueWrapper|GGBlockDevWriteAckQueueWrapper>blockdev_queue_reset",
      "~GGBlockDevWriteAckQueueWrapper|GGBlockDevWriteAckQueueWrapper>hostReset"};
  require(annos.size() == 4, "annotations lost");
  for (unsigned i = 0; i < 4; ++i) {
    auto a = cast<DictionaryAttr>(annos[i]);
    require(a.getAs<StringAttr>("target") == targets[i] && a.getAs<StringAttr>("class") == "test.Annotation", "annotation identity differs");
  }
  std::map<std::string, InstanceOp> instances;
  for (auto inst : top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  require(instances.size() == 2, "wrapper needs simulator and acknowledgment queue");
  auto sim = instances.at("sim"), fifo = instances.at("wAckBuf");
  require(fifo.getModuleName() == "GGBlockDevWriteAckQueue4", "acknowledgment queue helper differs");
  auto bulk = [&](Value dest, Value src) { for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true; return false; };
  auto wire = [&](Value dest, Value src) { for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true; return false; };
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(bulk(fifo.getResult(2), arg(4)) && bulk(sim.getResult(2), fifo.getResult(3)), "host enqueue or target dequeue direction differs");
  require(wire(fifo.getResult(0), arg(0)) && wire(fifo.getResult(1), sim.getResult(3)), "qualified reset or host clock differs");
  require(bulk(sim.getResult(0), arg(0)) && bulk(sim.getResult(1), arg(1)) &&
      bulk(arg(2), sim.getResult(3)) && bulk(arg(3), sim.getResult(4)), "copied reset or unrelated ports differ");
}
void rejection(MLIRContext &context) {
  constexpr unsigned invalidCases = 16;
  for (unsigned bad = 0; bad < invalidCases; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    auto top = named(c, "GGBlockDevReadResponseQueueWrapper"); OpBuilder b(&context);
    auto name = [&](unsigned index, llvm::StringRef value) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end()); names[index] = b.getStringAttr(value); top.setPortNames(names);
    };
    auto direction = [&](unsigned index) {
      SmallVector<bool> dirs(top.getPortDirections().begin(), top.getPortDirections().end()); dirs[index] = !dirs[index]; top.setPortDirections(dirs);
    };
    auto type = [&](unsigned index, Type value) {
      SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end()); types[index] = TypeAttr::get(value); top.setPortTypes(types);
    };
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2) name(2, "missing_acknowledgment_boundary");
    if (bad == 3) direction(2);
    if (bad >= 4 && bad <= 7) {
      auto bit = UIntType::get(&context, 1, false), wide = UIntType::get(&context, 2, false);
      auto token = BundleType::get(&context, {{b.getStringAttr("ready"), bad != 4, bad == 5 ? wide : bit},
          {b.getStringAttr("valid"), false, bad == 6 ? wide : bit}, {b.getStringAttr("bits"), false, bad == 7 ? wide : bit}});
      type(2, token);
    }
    if (bad == 8) type(0, UIntType::get(&context, 1, false));
    if (bad == 9) direction(0);
    if (bad == 10) direction(3);
    if (bad == 11) name(3, "missing_queue_reset");
    if (bad == 12) type(3, UIntType::get(&context, 2, false));
    if (bad == 13) name(4, "blockdev_wack_enq");
    if (bad == 14) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 15) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGBlockDevWriteAckQueue4"), top.getConventionAttr(), ArrayRef<PortInfo>{}); }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addBlockDevWriteAckQueue(c, error)) && !error.empty(), "invalid acknowledgment queue boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected acknowledgment mapping mutated IR");
  }
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevWriteAckQueue(c, error)), error);
  std::string before, after; { llvm::raw_string_ostream out(before); root->print(out); }
  require(failed(goldengate::addBlockDevWriteAckQueue(c, error)), "repeated acknowledgment mapping accepted");
  { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "repeated mapping mutated IR");
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); rejection(context);
    llvm::outs() << "Acknowledgment tag ordering, stalls, reset/write, async RAM, reverse wiring, annotations and 17 atomic rejections passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
