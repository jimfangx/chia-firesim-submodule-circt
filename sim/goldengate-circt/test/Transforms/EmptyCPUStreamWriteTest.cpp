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
    firrtl.circuit "GGCPUStreamControlWrapper" {
      firrtl.module @GGCPUStreamControlWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %cpu_stream_r_bits_id: !firrtl.uint<16>, out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n : {"cpu_stream_r_bits_id", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGCPUStreamControlWrapper|GGCPUStreamControlWrapper>" + std::string(n)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
// Interpret the generated FIRRTL, including assertion predicates. These
// assertions apply even though the empty incoming list never accepts writes.
struct ValueLess {
  bool operator()(Value a, Value b) const { return std::less<const void *>{}(a.getAsOpaquePointer(), b.getAsOpaquePointer()); }
};
struct Interpreter {
  FModuleOp module; SmallVector<AssertOp> assertions;
  std::map<Value, Value, ValueLess> drivers; std::map<Value, APInt, ValueLess> memo;
  Interpreter(FModuleOp m) : module(m) {
    for (auto &op : m.getBodyBlock()->getOperations())
      require(!isa<RegOp, RegResetOp, MemOp>(op), "empty write boundary has state");
    for (auto a : m.getOps<AssertOp>()) assertions.push_back(a);
    require(assertions.size() == 2, "two protocol assertions required");
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(c.getDest(), c.getSrc()).second, "multiple drivers");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(unsigned i, uint64_t n) { memo.insert_or_assign(arg(i), APInt(*cast<UIntType>(arg(i).getType()).getWidth(), n)); }
  APInt eval(Value v) {
    unsigned w = *cast<UIntType>(v.getType()).getWidth();
    if (memo.count(v)) return memo.at(v);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (drivers.count(v)) n = eval(drivers.at(v));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else throw std::runtime_error("unsupported operation or missing driver");
    memo.insert_or_assign(v, n); return n;
  }
};
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addEmptyCPUStreamWrite(c, error)), error);
  require(succeeded(verify(*root)), "IR invalid"); Interpreter sim(named(c, "GGEmptyCPUStreamWrite"));
  std::mt19937_64 rng(163); unsigned cases = 0;
  auto sample = [&](unsigned flags, unsigned id, unsigned size, uint64_t strobe) {
    sim.memo.clear(); sim.put(1, flags&1); sim.put(3, (flags>>1)&1); sim.put(4, id);
    sim.put(5, size); sim.put(7, (flags>>2)&1); sim.put(8, strobe);
    sim.put(12, (flags>>3)&1); // B-ready cannot activate an empty sink.
    for (unsigned i : {2, 6, 9, 11}) require(sim.eval(sim.arg(i)).isZero(), "empty interface accepts/responds to a write");
    require(sim.eval(sim.arg(10)).getZExtValue() == id, "B ID is not the live AW ID");
    for (unsigned i = 0; i < 2; ++i) {
      auto a = sim.assertions[i];
      require(a.getClock() == sim.arg(0), "assertion uses wrong clock");
      require(sim.eval(a.getEnable()).getBoolValue() == !(flags&1), "assertion enabled during reset");
      bool expected = i == 0 ? !(flags&2) || size == 6 : !(flags&4) || strobe == ~uint64_t(0);
      require(sim.eval(a.getPredicate()).getBoolValue() == expected, "protocol assertion differs");
    }
    ++cases;
  };
  for (unsigned flags = 0; flags < 16; ++flags) for (unsigned size = 0; size < 8; ++size) {
    sample(flags, rng()&65535, size, ~uint64_t(0)); sample(flags, rng()&65535, size, 0);
    for (unsigned lane = 0; lane < 64; ++lane) sample(flags, rng()&65535, size, ~(1ULL<<lane));
  }
  // Every ID while B invalid, including ID changes during reset and invalid AW.
  for (unsigned id = 0; id < 65536; ++id) sample(rng()&15, id, rng()&7, rng());
  llvm::outs() << "Empty CPU write: " << cases << " operation cases; all 64 strobe lanes and 65536 live IDs checked\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addEmptyCPUStreamWrite(c, error)), error);
  auto top = named(c, "GGCPUStreamWriteWrapper"), h = named(c, "GGEmptyCPUStreamWrite");
  require(top.getNumPorts() == 15 && h.getNumPorts() == 13, "boundary shape differs");
  require(h->getAttrOfType<IntegerAttr>("goldengate.fromHostCPUStreamCount").getInt() == 0, "empty stream metadata missing");
  InstanceOp sim, transport;
  for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == h.getName()) transport = i; else sim = i;
  require(sim && transport, "wrapper instances missing");
  auto wired = [&](Value d, Value s) {
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == d && conn.getSrc() == s) return true;
    return false;
  };
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(wired(transport.getResult(0), arg(0)) && wired(transport.getResult(1), arg(1)), "clock/reset disconnected");
  unsigned copies = 0;
  for (auto conn : top.getOps<ConnectOp>()) {
    unsigned i = copies++;
    bool input = i < 2;
    require(conn.getDest() == (input ? sim.getResult(i) : arg(i)) && conn.getSrc() == (input ? arg(i) : sim.getResult(i)), "existing boundary disconnected");
  }
  require(copies == 4, "copied ports lost");
  for (unsigned i = 2; i < 13; ++i) {
    require(top.getPortName(i+2) == "cpu_stream_" + h.getPortName(i).str() && top.getPortType(i+2) == h.getPortType(i) && top.getPortDirection(i+2) == h.getPortDirection(i), "write ports differ");
    require(h.getPortDirection(i) == Direction::In ? wired(transport.getResult(i), arg(i+2)) : wired(arg(i+2), transport.getResult(i)), "write port disconnected");
  }
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 3, "annotations lost");
  for (auto a : annos) require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with("~GGCPUStreamWriteWrapper|GGCPUStreamWriteWrapper>"), "copied target identity differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 9; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGCPUStreamControlWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop"); if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad >= 2 && bad <= 5) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 1 : bad == 3 ? 2 : 3] = b.getStringAttr(bad < 4 ? "WrongPort" : bad == 4 ? "cpu_stream_aw_valid" : "OTHER_from_cpu_stream"); top.setPortNames(names);
    }
    if (bad == 6) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad >= 7) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(bad == 7 ? "GGEmptyCPUStreamWrite" : "GGCPUStreamWriteWrapper"), top.getConventionAttr(), ArrayRef<PortInfo>{}); }
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addEmptyCPUStreamWrite(c, error)), "invalid boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); behavior(context); mapping(context); rejection(context); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
