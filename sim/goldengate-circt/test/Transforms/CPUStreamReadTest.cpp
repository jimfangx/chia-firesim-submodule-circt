// See LICENSE for license details.
#include "goldengate/TracerVTokenEngine.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef msg) { if (!ok) throw std::runtime_error(msg.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("module missing");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGTracerVStreamQueueWrapper" {
      firrtl.module @GGTracerVStreamQueueWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
        out %tracerv_stream_count: !firrtl.uint<13>, out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n : {"tracerv_stream.bits", "tracerv_stream_count", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGTracerVStreamQueueWrapper|GGTracerVStreamQueueWrapper>" + std::string(n)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
// Evaluate the emitted operations; expected burst completion below tracks the
// number of transferred payloads independently of the generated register logic.
struct Interpreter {
  FModuleOp module; RegResetOp counter; AssertOp assertion;
  std::map<std::string, Value> drivers; std::map<std::string, APInt> memo; unsigned state = 0;
  Interpreter(FModuleOp m) : module(m) {
    for (auto r : m.getOps<RegResetOp>()) { require(!counter, "extra register"); counter = r; }
    for (auto a : m.getOps<AssertOp>()) { require(!assertion, "extra assertion"); assertion = a; }
    require(counter && assertion && counter.getResult().getType() == UIntType::get(m.getContext(), 9, false), "state/assertion missing");
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
  }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(unsigned i, uint64_t n) { memo[key(arg(i))] = APInt(64, n); }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = *cast<UIntType>(v.getType()).getWidth();
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp>(op)) n = APInt(9, state);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  APInt output(unsigned i) { return eval(drivers.at(key(arg(i)))); }
  void edge() { state = eval(counter.getResetSignal()).isZero() ? eval(drivers.at(key(counter.getResult()))).getZExtValue() : eval(counter.getResetValue()).getZExtValue(); }
};
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addCPUStreamRead(c, error)), error); require(succeeded(verify(*root)), "IR invalid");
  Interpreter sim(named(c, "GGCPUStreamRead")); std::mt19937_64 rng(161); unsigned cases = 0, transfers = 0;
  auto sample = [&](unsigned beats, unsigned len, bool reset, bool ar, bool ready, bool valid, uint64_t addr, unsigned size) {
    sim.state = beats; sim.memo.clear(); sim.put(1, reset); sim.put(4, ar); sim.put(5, rng() & 65535);
    sim.put(6, addr); sim.put(7, len); sim.put(8, size); sim.put(9, ready);
    APInt data(512, 0); for (unsigned j = 0; j < 8; ++j) data |= APInt(512, rng()).shl(64*j);
    sim.memo[sim.key(sim.arg(2)) + ".valid"] = APInt(1, valid);
    sim.memo[sim.key(sim.arg(2)) + ".bits"] = data;
    bool grant = addr < (1ULL << 19), last = beats == len;
    require(sim.output(3).getBoolValue() == (grant && ready && valid && last), "AR ready differs (AR valid excluded)");
    require(sim.output(10).getBoolValue() == (grant && ar && valid), "R valid differs (R ready excluded)");
    require(sim.eval(sim.drivers.at(sim.key(sim.arg(2)) + ".ready")).getBoolValue() == (grant && ar && ready), "queue ready differs (queue valid excluded)");
    require(sim.output(11) == sim.eval(sim.arg(5)) && sim.output(12) == data && sim.output(13).getBoolValue() == last && sim.output(14).isZero(), "R payload/ID/resp gating differs");
    require(sim.eval(sim.assertion.getEnable()).getBoolValue() == !reset &&
        sim.eval(sim.assertion.getPredicate()).getBoolValue() == (!ar || size == 6), "size assertion differs");
    bool fire = grant && ar && ready && valid; transfers += fire; ++cases;
    sim.edge(); require(sim.state == (reset ? 0 : fire ? last ? 0 : (beats+1)&511 : beats), "counter next state differs");
    return fire;
  };
  // Every counter/length pair, including the ninth-bit states that cannot
  // complete against an 8-bit AR length; every handshake/reset/address flag.
  for (unsigned beats = 0; beats < 512; ++beats) for (unsigned len = 0; len < 256; ++len)
    sample(beats, len, rng()&1, rng()&1, rng()&1, rng()&1, rng()&1 ? rng()%(1ULL<<19) : rng(), rng()&7);
  for (unsigned beats : {0, 1, 255, 256, 511}) for (unsigned len : {0, 1, 254, 255})
    for (unsigned flags = 0; flags < 32; ++flags)
      sample(beats, len, flags&1, flags&2, flags&4, flags&8, flags&16 ? 1ULL<<19 : (1ULL<<19)-1, flags&7);
  // Legal held-AR transfers, gaps from the producer and consumer, address misses
  // and reset during a stalled burst. No AR capture or skid buffer is allowed.
  for (unsigned len = 0; len < 256; ++len) {
    unsigned sent = 0; sim.state = 0;
    while (sent <= len) {
      unsigned old = sim.state;
      bool fire = sample(old, len, false, true, rng()&1, rng()&1, rng()%(1ULL<<19), 6);
      sent += fire; require(sim.state == (sent <= len ? sent : 0), "held-AR burst length differs");
    }
  }
  sample(100, 255, true, true, true, true, 0, 6);
  llvm::outs() << "CPU stream read: " << cases << " operation cases, " << transfers << " transferred beats; all 256 burst lengths checked\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addCPUStreamRead(c, error)), error);
  auto top = named(c, "GGCPUStreamReadWrapper"); require(top.getNumPorts() == 16 && top.getPortName(2) == "tracerv_stream_count", "wrapper ports differ");
  for (auto p : top.getPorts()) require(p.name != "tracerv_stream", "buffered stream still exposed");
  auto h = named(c, "GGCPUStreamRead"); require(h.getNumPorts() == 15 && h->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == 19, "transport shape differs");
  InstanceOp sim, transport;
  for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == h.getName()) transport = i; else sim = i;
  require(sim && transport, "wrapper instances missing");
  auto wired = [&](Value d, Value s) { for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == d && conn.getSrc() == s) return true; return false; };
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(wired(transport.getResult(0), arg(0)) && wired(transport.getResult(1), arg(1)), "clock/reset disconnected");
  bool stream = false; for (auto conn : top.getOps<ConnectOp>()) stream |= conn.getDest() == transport.getResult(2) && conn.getSrc() == sim.getResult(2);
  require(stream, "queue not connected");
  for (unsigned i = 3; i < 15; ++i) {
    require(top.getPortName(i+1) == "cpu_stream_" + h.getPortName(i).str() && top.getPortType(i+1) == h.getPortType(i) && top.getPortDirection(i+1) == h.getPortDirection(i), "AXI ports differ");
    require(h.getPortDirection(i) == Direction::In ? wired(transport.getResult(i), arg(i+1)) : wired(arg(i+1), transport.getResult(i)), "AXI disconnected");
  }
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 4, "annotations lost");
  for (unsigned i = 0; i < 4; ++i) {
    auto target = cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue();
    require(target.starts_with(i == 0 ? "~GGCPUStreamReadWrapper|GGTracerVStreamQueueWrapper>" : "~GGCPUStreamReadWrapper|GGCPUStreamReadWrapper>"), "annotation target differs");
  }
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 8; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGTracerVStreamQueueWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop"); if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad >= 2 && bad <= 4) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 2 : bad == 3 ? 3 : 4] = b.getStringAttr(bad == 4 ? "cpu_stream_ar_valid" : "WrongPort"); top.setPortNames(names);
    }
    if (bad == 5) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad >= 6) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(bad == 6 ? "GGCPUStreamRead" : "GGCPUStreamReadWrapper"), top.getConventionAttr(), ArrayRef<PortInfo>{}); }
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addCPUStreamRead(c, error)), "invalid boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); behavior(context); mapping(context); rejection(context); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
