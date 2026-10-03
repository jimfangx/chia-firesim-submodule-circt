// See LICENSE for license details.
#include "goldengate/LoadMemWriter.h"
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
    if (m.getOps<MemOp>().empty()) return;
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
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
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
    firrtl.circuit "GGLoadMemWriterWrapper" {
      firrtl.module @GGLoadMemWriterWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %loadmem_req_ready: !firrtl.uint<1>, in %loadmem_req_valid: !firrtl.uint<1>,
        in %loadmem_req_bits_zero: !firrtl.uint<1>, in %loadmem_req_bits_addr: !firrtl.uint<34>,
        in %loadmem_req_bits_len: !firrtl.uint<34>, out %other: !firrtl.uint<8>,
        in %loadmem_data_valid: !firrtl.uint<1>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n : {"loadmem_req_valid", "loadmem_req_bits_addr", "other", "hostReset", "loadmem_data_valid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGLoadMemWriterWrapper|GGLoadMemWriterWrapper>" + std::string(n)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemRequests(c, error)), error);
  require(succeeded(verify(*root)), "request IR verification failed");
  Interpreter sim(named(c, "GGLoadMemRequestQueue2")), arb(named(c, "GGLoadMemRequestArbiter"));
  require(sim.widths == std::map<std::string, unsigned>{{"zero", 1}, {"addr", 34}, {"len", 34}}, "wrong request payload");
  std::deque<std::map<std::string, APInt>> expected;
  std::mt19937_64 random(169);
  unsigned pushes = 0, pops = 0, zeroFires = 0, blockedZero = 0, resetWrites = 0, fullBlocked = 0;
  auto cycle = [&](bool reset, bool valid, bool zero, bool ready) {
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = APInt(64, reset);
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = APInt(64, valid);
    std::map<std::string, APInt> data;
    for (auto f : sim.widths) {
      APInt n(64, f.first == "zero" ? 0 : random()); n = n.zextOrTrunc(f.second).zextOrTrunc(64);
      sim.memo[sim.key(sim.arg(2)) + ".bits." + f.first] = data[f.first] = n;
    }
    bool qvalid = !sim.drive(sim.arg(3), "valid").isZero();
    arb.memo.clear();
    arb.memo[arb.key(arb.arg(1))] = APInt(64, qvalid);
    arb.memo[arb.key(arb.arg(6))] = APInt(64, zero);
    arb.memo[arb.key(arb.arg(7))] = APInt(64, ready);
    const char *fields[]{"zero", "addr", "len"};
    for (unsigned i = 0; i < 3; ++i) arb.memo[arb.key(arb.arg(i + 2))] = sim.drive(sim.arg(3), "bits." + std::string(fields[i]));
    auto out = [&](unsigned i) { return arb.eval(arb.arg(i)).getZExtValue(); };
    sim.memo[sim.key(sim.arg(3)) + ".ready"] = APInt(64, out(0));
    bool enqReady = !sim.drive(sim.arg(2), "ready").isZero();
    require(enqReady == (expected.size() < 2) && qvalid == !expected.empty(), "request occupancy differs");
    require(out(0) == ready && out(5) == (ready && expected.empty()) && out(8) == (!expected.empty() || zero), "zero priority/handshake differs");
    if (!expected.empty()) for (unsigned i = 0; i < 3; ++i)
      require(out(i + 9) == expected.front().at(fields[i]).getZExtValue(), "write reordered or bypassed by zero");
    else require(out(9) == 1 && out(10) == 0 && out(11) == (uint64_t(1) << 31), "zero fill payload differs");
    bool push = valid && enqReady, pop = ready && qvalid;
    auto before = sim.memory; unsigned address = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    if (pop) expected.pop_front(); if (push) expected.push_back(data); if (reset) expected.clear(); sim.edge();
    for (auto item : sim.widths) for (unsigned i = 0; i < 2; ++i)
      require(sim.memory[item.first][i] == (push && i == address ? data[item.first] : before[item.first][i]), "reset-time request RAM writes differ");
    pushes += push; pops += pop; zeroFires += zero && out(5); blockedZero += zero && qvalid;
    resetWrites += reset && push; fullBlocked += !enqReady && valid && ready;
  };
  for (unsigned i = 0; i < 100; ++i) { cycle(true, true, true, false); cycle(false, true, true, false); cycle(false, true, true, false); cycle(false, true, true, true); }
  for (unsigned i = 0; i < 20000; ++i) cycle(random()%23 == 0, random()&1, random()&1, random()&1);
  require(pushes > 1000 && pops > 1000 && zeroFires > 100 && blockedZero > 100 && resetWrites > 100 && fullBlocked > 100, "insufficient request coverage");
  // Also exercise arbitrary stored zero bits (including invalid payload), all
  // Boolean inputs, and full-width addr/len directly in the actual arbiter IR.
  for (unsigned i = 0; i < 4096; ++i) {
    arb.memo.clear(); bool v0 = i&1, v1 = i&2, ready = i&4, zero = i&8;
    uint64_t addr = random() & ((uint64_t(1)<<34)-1), len = random() & ((uint64_t(1)<<34)-1);
    for (auto v : std::map<unsigned, uint64_t>{{1,v0},{2,zero},{3,addr},{4,len},{6,v1},{7,ready}})
      arb.memo[arb.key(arb.arg(v.first))] = APInt(64,v.second);
    auto out = [&](unsigned j) { return arb.eval(arb.arg(j)).getZExtValue(); };
    require(out(0)==ready && out(5)==(!v0 && ready) && out(8)==(v0 || v1) &&
      out(9)==(v0 ? zero : 1) && out(10)==(v0 ? addr : 0) && out(11)==(v0 ? len : uint64_t(1)<<31), "arbiter truth table differs");
  }
  llvm::outs() << "LoadMem requests: 20400 cycles, pushes " << pushes << ", pops " << pops << ", zero fires " << zeroFires
    << ", zero blocked " << blockedZero << ", reset writes " << resetWrites << ", full blocked " << fullBlocked << "; 4096 arbiter cases\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto before = named(c, "GGLoadMemWriterWrapper");
  require(succeeded(goldengate::addLoadMemRequests(c, error)), error);
  require(succeeded(verify(*root)), "request wrapper verification failed"); auto top = named(c, "GGLoadMemRequestWrapper");
  require(top.getNumPorts() == before.getNumPorts() + 2, "wrong request wrapper geometry");
  std::map<std::string, InstanceOp> instances;
  for (auto i : top.getOps<InstanceOp>()) instances.emplace(i.getName().str(), i);
  require(instances.size() == 3, "missing request instances"); auto sim = instances.at("sim"), q = instances.at("writeRequests"), a = instances.at("requestArbiter");
  require(q.getModuleName() == "GGLoadMemRequestQueue2" && a.getModuleName() == "GGLoadMemRequestArbiter", "wrong request helpers");
  Interpreter keys(named(c, "GGLoadMemRequestQueue2"));
  std::map<std::string, std::string> wires;
  for (auto conn : top.getOps<StrictConnectOp>()) require(wires.emplace(keys.key(conn.getDest()), keys.key(conn.getSrc())).second, "duplicate request driver");
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  auto wire = [&](std::string d, std::string s) { require(wires.at(d) == s, "request binding differs"); };
  wire(keys.key(q.getResult(0)), keys.key(arg(0))); wire(keys.key(q.getResult(1)), keys.key(arg(1)));
  unsigned first = 4;
  wire(keys.key(arg(first)), keys.key(q.getResult(2)) + ".ready");
  wire(keys.key(q.getResult(2)) + ".valid", keys.key(arg(first + 1)));
  wire(keys.key(q.getResult(2)) + ".bits.addr", keys.key(arg(first + 2)));
  wire(keys.key(q.getResult(2)) + ".bits.len", keys.key(arg(first + 3)));
  auto zeroKey = wires.at(keys.key(q.getResult(2)) + ".bits.zero"); bool zeroConstant = false;
  for (auto n : top.getOps<ConstantOp>()) if (keys.key(n.getResult()) == zeroKey) zeroConstant = n.getValue().isZero();
  require(zeroConstant, "write requests do not store zero=0");
  wire(keys.key(q.getResult(3)) + ".ready", keys.key(a.getResult(0)));
  wire(keys.key(a.getResult(1)), keys.key(q.getResult(3)) + ".valid");
  const char *fields[]{"zero", "addr", "len"};
  for (unsigned i = 0; i < 3; ++i) wire(keys.key(a.getResult(i + 2)), keys.key(q.getResult(3)) + ".bits." + fields[i]);
  wire(keys.key(arg(first + 4)), keys.key(a.getResult(5))); wire(keys.key(a.getResult(6)), keys.key(arg(first + 5)));
  wire(keys.key(a.getResult(7)), keys.key(sim.getResult(2))); wire(keys.key(sim.getResult(3)), keys.key(a.getResult(8)));
  for (unsigned i = 0; i < 3; ++i) wire(keys.key(sim.getResult(i + 4)), keys.key(a.getResult(i + 9)));
  wire(keys.key(arg(first + 6)), keys.key(sim.getResult(2)));
  unsigned copied = 0;
  for (auto conn : top.getOps<ConnectOp>()) for (unsigned i : {0u, 1u, 7u, 8u}) {
    unsigned j = i < 2 ? i : i - 5;
    require(top.getPortName(j)==before.getPortName(i) && top.getPortType(j)==before.getPortType(i) && top.getPortDirection(j)==before.getPortDirection(i), "copied geometry differs");
    if (before.getPortDirection(i)==Direction::In && conn.getDest()==sim.getResult(i) && conn.getSrc()==arg(j)) ++copied;
    if (before.getPortDirection(i)==Direction::Out && conn.getDest()==arg(j) && conn.getSrc()==sim.getResult(i)) ++copied;
  }
  require(copied == 4, "lost copied port");
  for (auto [i, n] : llvm::enumerate(c->getAttrOfType<ArrayAttr>("rawAnnotations"))) {
    auto t = cast<DictionaryAttr>(n).getAs<StringAttr>("target").getValue();
    require(t.starts_with("~GGLoadMemRequestWrapper|"), "circuit identity differs");
    require(t.contains(i < 2 ? "|GGLoadMemWriterWrapper>" : "|GGLoadMemRequestWrapper>"), "consumed/copied target differs");
  }
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 11; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGLoadMemWriterWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 3 || bad == 7 || bad == 10) { SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      unsigned i = bad == 2 ? 5 : bad == 7 ? 0 : 7;
      names[i] = b.getStringAttr(bad == 3 ? "loadmem_req_bits_extra" : bad == 10 ? "loadmem_zero_valid" : "WrongField"); top.setPortNames(names); }
    if (bad == 4) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 5 || bad == 6 || bad == 9) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),
      b.getStringAttr(bad == 5 ? "GGLoadMemRequestWrapper" : bad == 6 ? "GGLoadMemRequestQueue2" : "GGLoadMemRequestArbiter"), top.getConventionAttr(), ArrayRef<PortInfo>{}); }
    if (bad == 8) { SmallVector<Attribute> types(top.getPortTypes().begin(), top.getPortTypes().end());
      types[5] = TypeAttr::get(UIntType::get(&context, 32, false)); top.setPortTypes(types); }
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addLoadMemRequests(c, error)), "unsupported request boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "request rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); behavior(context); mapping(context); rejection(context); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
