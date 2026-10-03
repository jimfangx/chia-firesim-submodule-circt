// See LICENSE for license details.
#include "goldengate/TracerVTokenEngine.h"
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
  std::map<std::string, std::array<APInt, 2>> memory;
  std::map<std::string, unsigned> widths;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 2 && isa<BundleType>(ram.getDataType()) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    unsigned regs = 0;
    for (auto reg : m.getOps<RegResetOp>()) { require(reg.getResult().getType() == UIntType::get(m.getContext(), 1, false), "wrong pointer width"); state[reg.getResult()] = APInt(64, 0); ++regs; }
    require(regs == 3, "queue needs three control bits");
    for (auto f : cast<BundleType>(ram.getDataType()).getElements()) {
      widths[f.name.str()] = *cast<UIntType>(f.type).getWidth();
      memory[f.name.str()] = {APInt(64, 0), APInt(64, 1)};
    }
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  APInt drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  APInt eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); APInt n(64, 0);
    if (isa_and_nonnull<RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().zextOrTrunc(64);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(64, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(!eval(op->getOperand(0)).isZero() ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(bits.getHi() - bits.getLo() + 1).zextOrTrunc(64);
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      auto parent = f.getInput().getDefiningOp<SubfieldOp>();
      require(parent && parent.getInput() == ram.getResult(0) && parent.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en") == APInt(64, 1), "queue read disabled");
      n = memory.at(f.getFieldName().str()).at(drive(ram.getResult(0), "addr").getZExtValue());
    } else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = *cast<UIntType>(v.getType()).getWidth();
    n = n.zextOrTrunc(width).zextOrTrunc(64);
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = !eval(r.getResetSignal()).isZero() ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    Value writer = ram.getResult(1);
    if (!drive(writer, "en").isZero()) for (auto item : widths) {
      require(drive(writer, "mask." + item.first) == APInt(64, 1), "payload mask disabled");
      memory.at(item.first).at(drive(writer, "addr").getZExtValue()) = drive(writer, "data." + item.first);
    }
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGCPUStreamResponseBufferWrapper" {
      firrtl.module @GGCPUStreamResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %cpu_stream_b_ready: !firrtl.uint<1>, out %cpu_stream_b_valid: !firrtl.uint<1>,
        out %cpu_stream_b_bits_id: !firrtl.uint<16>, out %cpu_stream_b_bits_resp: !firrtl.uint<2>,
        out %other: !firrtl.uint<8>, in %cpu_stream_ar_valid: !firrtl.uint<1>,
        out %cpu_stream_ar_ready: !firrtl.uint<1>, in %cpu_stream_aw_bits_id: !firrtl.uint<16>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n : {"cpu_stream_b_valid", "cpu_stream_b_bits_id", "other", "hostReset", "cpu_stream_ar_valid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGCPUStreamResponseBufferWrapper|GGCPUStreamResponseBufferWrapper>" + std::string(n)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addCPUStreamWriteResponseBuffer(c, error)), error);
  require(succeeded(verify(*root)), "B queue IR verification failed");
  Interpreter sim(named(c, "GGCPUStreamBQueue2"));
  require(sim.widths == std::map<std::string, unsigned>{{"id", 16}, {"resp", 2}}, "wrong B payload geometry");
  std::deque<std::map<std::string, APInt>> expected;
  std::mt19937_64 random(167);
  unsigned pushes = 0, pops = 0, blocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0;
  auto cycle = [&](bool reset, bool valid, bool ready) {
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = APInt(64, reset);
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = APInt(64, valid);
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = APInt(64, ready);
    std::map<std::string, APInt> data;
    for (auto f : sim.widths) {
      uint64_t words[1]; for (auto &word : words) word = random();
      APInt n(64, ArrayRef<uint64_t>(words)); n = n.zextOrTrunc(f.second).zextOrTrunc(64);
      sim.memo[sim.key(sim.arg(2)) + ".bits." + f.first] = data[f.first] = n;
    }
    bool enqReady = !sim.drive(sim.arg(2), "ready").isZero(), deqValid = !sim.drive(sim.arg(3), "valid").isZero();
    require(enqReady == (expected.size() < 2) && deqValid == !expected.empty(), "queue occupancy differs");
    bool push = valid && enqReady, pop = ready && deqValid;
    if (deqValid) for (auto item : sim.widths)
      require(sim.drive(sim.arg(3), "bits." + item.first) == expected.front().at(item.first), "B id/response reordered");
    auto before = sim.memory; unsigned address = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    if (pop) expected.pop_front(); if (push) expected.push_back(data); if (reset) expected.clear(); sim.edge();
    for (auto item : sim.widths) for (unsigned i = 0; i < 2; ++i)
      require(sim.memory[item.first][i] == (push && i == address ? data[item.first] : before[item.first][i]), "RAM reset/write semantics differ");
    pushes += push; pops += pop; blocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    resetWrites += reset && push; simultaneous += push && pop;
  };
  // Stall host dequeue, fill two beats, and hold the first complete payload.
  for (unsigned i = 0; i < 100; ++i) { cycle(true, true, false); cycle(false, true, false); cycle(false, true, false); cycle(false, true, false); }
  for (unsigned i = 0; i < 20000; ++i) cycle(random()%23 == 0, random()&1, random()&1);
  require(pushes > 1000 && pops > 1000 && blocked > 100 && emptyBlocked > 100 && resetWrites > 100 && simultaneous > 100, "insufficient B queue coverage");
  llvm::outs() << "B queue: 20400 cycles, pushes " << pushes << ", pops " << pops << ", reset writes " << resetWrites << ", full/empty blocked " << blocked << "/" << emptyBlocked << "\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto before = named(c, "GGCPUStreamResponseBufferWrapper");
  require(succeeded(goldengate::addCPUStreamWriteResponseBuffer(c, error)), error);
  require(succeeded(verify(*root)), "B wrapper verification failed"); auto top = named(c, "GGCPUStreamWriteResponseBufferWrapper");
  require(top.getNumPorts() == before.getNumPorts(), "B buffer changed port count");
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    require(top.getPortName(i) == before.getPortName(i) && top.getPortType(i) == before.getPortType(i) &&
        top.getPortDirection(i) == before.getPortDirection(i), "B buffer changed external geometry");
  std::map<std::string, InstanceOp> instances;
  for (auto i : top.getOps<InstanceOp>()) instances.emplace(i.getName().str(), i);
  require(instances.size() == 2, "missing B queue instances"); auto sim = instances.at("sim"), r = instances.at("bQueue");
  require(r.getModuleName() == "GGCPUStreamBQueue2", "wrong B helper"); Interpreter keys(named(c, "GGCPUStreamBQueue2"));
  std::map<std::string, std::string> wires;
  for (auto conn : top.getOps<StrictConnectOp>())
    require(wires.emplace(keys.key(conn.getDest()), keys.key(conn.getSrc())).second, "duplicate B driver");
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  auto wire = [&](std::string d, std::string s) { require(wires.at(d) == s, "B wrapper connection differs"); };
  wire(keys.key(r.getResult(0)), keys.key(arg(0))); wire(keys.key(r.getResult(1)), keys.key(arg(1)));
  wire(keys.key(sim.getResult(2)), keys.key(r.getResult(2)) + ".ready");
  wire(keys.key(r.getResult(2)) + ".valid", keys.key(sim.getResult(3)));
  wire(keys.key(r.getResult(3)) + ".ready", keys.key(arg(2)));
  wire(keys.key(arg(3)), keys.key(r.getResult(3)) + ".valid");
  const char *fields[]{"id", "resp"};
  for (unsigned i = 0; i < 2; ++i) {
    wire(keys.key(r.getResult(2)) + ".bits." + fields[i], keys.key(sim.getResult(i + 4)));
    wire(keys.key(arg(i + 4)), keys.key(r.getResult(3)) + ".bits." + fields[i]);
  }
  unsigned copied = 0;
  for (auto conn : top.getOps<ConnectOp>()) for (unsigned i = 0; i < top.getNumPorts(); ++i) {
    if (i >= 2 && i <= 5) continue;
    if (top.getPortDirection(i) == Direction::In && conn.getDest() == sim.getResult(i) && conn.getSrc() == arg(i)) ++copied;
    if (top.getPortDirection(i) == Direction::Out && conn.getDest() == arg(i) && conn.getSrc() == sim.getResult(i)) ++copied;
  }
  require(copied == top.getNumPorts() - 4, "lost copied port");
  for (auto [i, a] : llvm::enumerate(c->getAttrOfType<ArrayAttr>("rawAnnotations"))) {
    auto target = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
    require(target.starts_with("~GGCPUStreamWriteResponseBufferWrapper|"), "circuit identity differs");
    require(target.contains(i < 2 ? "|GGCPUStreamResponseBufferWrapper>" : "|GGCPUStreamWriteResponseBufferWrapper>"), "consumed/copied identity differs");
  }
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 9; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGCPUStreamResponseBufferWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 3) { SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 4 : 6] = b.getStringAttr(bad == 2 ? "WrongID" : "cpu_stream_b_bits_user"); top.setPortNames(names); }
    if (bad == 4) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 5 || bad == 6) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),
      b.getStringAttr(bad == 5 ? "GGCPUStreamWriteResponseBufferWrapper" : "GGCPUStreamBQueue2"), top.getConventionAttr(), ArrayRef<PortInfo>{}); }
    if (bad == 7) { SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[0] = b.getStringAttr("WrongClock"); top.setPortNames(names); }
    if (bad == 8) { SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[4] = TypeAttr::get(UIntType::get(&context, 8, false)); top.setPortTypes(types); }
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addCPUStreamWriteResponseBuffer(c, error)), "unsupported B boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "B rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); behavior(context); mapping(context); rejection(context); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
