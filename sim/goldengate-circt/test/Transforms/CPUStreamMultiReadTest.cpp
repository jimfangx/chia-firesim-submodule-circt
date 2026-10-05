// See LICENSE for license details.
#include "goldengate/CPUStreamRead.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
using Spec = goldengate::CPUStreamSourcePort;
void require(bool ok, llvm::StringRef message) { if (!ok) throw std::runtime_error(message.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module " + name.str());
}
std::string dump(Operation *op) { std::string text; llvm::raw_string_ostream out(text); op->print(out); return text; }
const SmallVector<Spec> specs{{"print", "sourceB", 3}, {"tracerv", "sourceA", 6144}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "StreamFixture" {
    firrtl.module @StreamFixture(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %validA: !firrtl.uint<1>, in %dataA: !firrtl.uint<512>,
      in %validB: !firrtl.uint<1>, in %dataB: !firrtl.uint<512>, in %payload: !firrtl.uint<8>,
      out %sourceA: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %sourceB: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %other: !firrtl.uint<8>) {}
  } })", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "StreamFixture");
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  for (unsigned i = 0; i < 2; ++i) {
    Value stream = top.getArgument(7 + i);
    b.create<StrictConnectOp>(top.getLoc(), b.create<SubfieldOp>(top.getLoc(), stream, "valid"), top.getArgument(2 + 2 * i));
    b.create<StrictConnectOp>(top.getLoc(), b.create<SubfieldOp>(top.getLoc(), stream, "bits"), top.getArgument(3 + 2 * i));
  }
  b.create<StrictConnectOp>(top.getLoc(), top.getArgument(9), top.getArgument(6));
  SmallVector<Attribute> annos;
  for (auto target : {"~StreamFixture", "~StreamFixture|StreamFixture>sourceA.bits", "~StreamFixture|StreamFixture>sourceB.valid",
       "~StreamFixture|StreamFixture>other", "~StreamFixture|StreamFixture>hostReset", "~StreamFixture|StreamFixture/leaf:Leaf>foo", "~Foreign|Foreign>sourceA"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.StreamAnnotation")), b.getNamedAttr("target", b.getStringAttr(target))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
struct Interpreter {
  FModuleOp module; SmallVector<RegResetOp> regs;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, unsigned> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto r : m.getOps<RegResetOp>()) { regs.push_back(r); require(r.getResult().getType() == UIntType::get(m.getContext(), 9, false), "wrong burst counter type"); }
    require(regs.size() == 2 && regs[0].getName() == "readBeatCounter_0" && regs[1].getName() == "readBeatCounter_1", "independent per-stream counters missing");
    for (auto op : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(op.getDest()), op.getSrc()).second, "multiple transport drivers");
  }
  Value arg(unsigned i) { return module.getArgument(i); }
  void put(unsigned i, uint64_t n) { memo[key(arg(i))] = APInt(64, n); }
  APInt eval(Value v) {
    auto k = key(v); unsigned width = *cast<UIntType>(v.getType()).getWidth();
    if (memo.count(k)) return memo.at(k).zextOrTrunc(width);
    auto *op = v.getDefiningOp(); APInt n(width, 0);
    if (isa_and_nonnull<RegResetOp>(op)) n = APInt(9, state.lookup(v));
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo());
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(width) + eval(op->getOperand(1)).zextOrTrunc(width);
    else throw std::runtime_error("unsupported transport SSA operation or missing driver");
    n = n.zextOrTrunc(width); memo[k] = n; return n;
  }
  APInt output(unsigned i) { return eval(drivers.at(key(arg(i)))); }
  void edge() {
    llvm::DenseMap<Value, unsigned> next;
    for (auto r : regs) next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))).getZExtValue() : eval(r.getResetValue()).getZExtValue();
    state = std::move(next);
  }
};
void behavior(CircuitOp c) {
  Interpreter sim(named(c, "GGCPUStreamRead")); std::mt19937_64 rng(0x1373);
  unsigned cases = 0, fires = 0, unselectedUpdates = 0, misses = 0;
  auto cycle = [&](std::array<unsigned, 2> old, unsigned len, uint64_t addr, unsigned flags, unsigned size) {
    bool reset = flags & 1, ar = flags & 2, ready = flags & 4;
    bool valids[]{bool(flags & 8), bool(flags & 16)};
    sim.memo.clear(); for (unsigned i = 0; i < 2; ++i) sim.state[sim.regs[i].getResult()] = old[i];
    sim.put(1, reset); sim.put(5, ar); sim.put(6, rng() & 65535); sim.put(7, addr); sim.put(8, len); sim.put(9, size); sim.put(10, ready);
    std::array<APInt, 2> data{APInt(512, 0), APInt(512, 0)};
    for (unsigned i = 0; i < 2; ++i) {
      for (unsigned word = 0; word < 8; ++word) data[i] |= APInt(512, rng()).shl(64 * word);
      sim.memo[sim.key(sim.arg(2 + i)) + ".valid"] = APInt(1, valids[i]);
      sim.memo[sim.key(sim.arg(2 + i)) + ".bits"] = data[i];
    }
    uint64_t selector = addr >> 19; bool selected = selector < 2;
    bool valid = selected && ar && valids[selector], fire = valid && ready;
    bool last = selected && old[selector] == len;
    require(sim.output(4).getBoolValue() == (selected && ready && valids[selector] && last), "AR ready predicate differs");
    require(sim.output(11).getBoolValue() == valid && sim.output(12) == sim.eval(sim.arg(6)) && sim.output(15).isZero(), "R valid/ID/response differs");
    require(sim.output(13) == (selected ? data[selector] : APInt(512, 0)) && sim.output(14).getBoolValue() == last, "selected R data/last or miss defaults differ");
    for (unsigned i = 0; i < 2; ++i)
      require(sim.eval(sim.drivers.at(sim.key(sim.arg(2 + i)) + ".ready")).getBoolValue() == (selector == i && ar && ready), "producer ready differs");
    unsigned assertions = 0;
    for (auto a : sim.module.getOps<AssertOp>()) {
      require(sim.eval(a.getEnable()).getBoolValue() == !reset && sim.eval(a.getPredicate()).getBoolValue() == (!ar || size == 6), "size assertion/reset gating differs"); ++assertions;
    }
    require(assertions == 1, "size assertion must be shared across streams");
    sim.edge();
    for (unsigned i = 0; i < 2; ++i) {
      unsigned expected = reset ? 0 : fire ? (old[i] == len ? 0 : (old[i] + 1) & 511) : old[i];
      require(sim.state.lookup(sim.regs[i].getResult()) == expected, "global R.fire must update even the unselected stream counter");
    }
    ++cases; fires += fire; unselectedUpdates += fire && !reset; misses += !selected; return fire;
  };
  // Different seeds expose accidental grant gating of the unselected counter.
  for (unsigned seed = 0; seed < 512; ++seed) for (unsigned len : {0U, 1U, 127U, 255U})
    for (unsigned flags = 0; flags < 32; ++flags)
      cycle({seed, (seed + 173) & 511}, len, (uint64_t(flags % 3) << 19) | ((1U << 19) - 1), flags, flags & 7);
  cycle({511, 256}, 255, ~uint64_t(0), 30, 6);
  // Held AR bursts of all legal lengths; alternate selected stream between bursts.
  for (unsigned len = 0; len < 256; ++len) {
    std::array<unsigned, 2> counts{0, 0}; unsigned sent = 0;
    while (sent <= len) {
      unsigned flags = 2 | (rng() & 1 ? 4 : 0) | (rng() & 1 ? 8 : 0) | (rng() & 1 ? 16 : 0);
      bool fire = cycle(counts, len, (uint64_t(len & 1) << 19) | (rng() & ((1U << 19) - 1)), flags, 6);
      sent += fire; counts = {sim.state.lookup(sim.regs[0].getResult()), sim.state.lookup(sim.regs[1].getResult())};
      require(counts[0] == counts[1] && counts[0] == (sent <= len ? sent : 0), "held-AR burst completion differs");
    }
  }
  require(unselectedUpdates && misses, "global-fire/miss coverage missing");
  llvm::outs() << "Multi-stream read: " << cases << " SSA cases, " << fires << " R transfers, " << unselectedUpdates << " unselected counter updates\n";
}
void mapping(CircuitOp c) {
  auto helper = named(c, "GGCPUStreamRead"), wrapper = named(c, "GGCPUStreamReadWrapper"), inner = named(c, "StreamFixture");
  require(c.getName() == wrapper.getName() && helper.getNumPorts() == 16 && wrapper.getNumPorts() == 20, "multi-stream interface shape differs");
  require(helper.getPortName(2) == "stream_0" && helper.getPortName(3) == "stream_1", "source index naming differs");
  for (auto module : {helper}) {
    auto addressBits = module->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits");
    require(addressBits && addressBits.getInt() == 19, "common address window width differs");
    auto sources = module->getAttrOfType<ArrayAttr>("goldengate.sourceStreams"); require(sources && sources.size() == 2, "ordered source collateral missing");
    for (auto [i, spec] : llvm::enumerate(specs)) { auto source = cast<DictionaryAttr>(sources[i]);
      require(source.getAs<StringAttr>("name").getValue() == spec.streamName && source.getAs<StringAttr>("port").getValue() == spec.portName &&
          source.getAs<IntegerAttr>("index").getInt() == i && source.getAs<IntegerAttr>("depth").getInt() == spec.depth &&
          source.getAs<IntegerAttr>("widthBytes").getInt() == 64 && source.getAs<IntegerAttr>("bufferBaseAddress").getInt() == (i << 19), "stream window metadata/order differs");
    }
  }
  InstanceOp sim, transport;
  for (auto i : wrapper.getOps<InstanceOp>()) if (i.getModuleName() == helper.getName()) transport = i; else sim = i;
  require(sim && transport, "missing wrapper instances"); llvm::DenseMap<Value, Value> drivers;
  for (auto con : wrapper.getOps<ConnectOp>()) drivers[con.getDest()] = con.getSrc();
  for (auto con : wrapper.getOps<StrictConnectOp>()) drivers[con.getDest()] = con.getSrc();
  require(drivers.lookup(transport.getResult(2)) == sim.getResult(8) && drivers.lookup(transport.getResult(3)) == sim.getResult(7), "source wiring ignored supplied order");
  for (unsigned i = 0; i < 8; ++i) { unsigned original = i < 7 ? i : 9;
    require(wrapper.getPortName(i) == inner.getPortName(original) && wrapper.getPortType(i) == inner.getPortType(original), "retained port changed");
  }
  for (unsigned i = 4; i < 16; ++i) {
    unsigned external = i + 4;
    require(wrapper.getPortName(external) == "cpu_stream_" + helper.getPortName(i).str(), "AXI scalar port order differs");
    require(helper.getPortDirection(i) == Direction::In ? drivers.lookup(transport.getResult(i)) == wrapper.getArgument(external) : drivers.lookup(wrapper.getArgument(external)) == transport.getResult(i), "AXI port disconnected");
  }
  const char *expected[]{"~GGCPUStreamReadWrapper", "~GGCPUStreamReadWrapper|StreamFixture>sourceA.bits", "~GGCPUStreamReadWrapper|StreamFixture>sourceB.valid",
      "~GGCPUStreamReadWrapper|GGCPUStreamReadWrapper>other", "~GGCPUStreamReadWrapper|GGCPUStreamReadWrapper>hostReset",
      "~GGCPUStreamReadWrapper|StreamFixture/leaf:Leaf>foo", "~Foreign|Foreign>sourceA"};
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 7, "annotation lost");
  for (unsigned i = 0; i < 7; ++i) require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue() == expected[i], "retargeted annotation identity differs");
}
void reject(MLIRContext &ctx, ArrayRef<Spec> sources, std::function<void(CircuitOp)> mutate = {}) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); if (mutate) mutate(c);
  auto before = dump(*root); std::string error;
  require(failed(goldengate::addCPUStreamRead(c, sources, error)) && !error.empty() && dump(*root) == before, "bad source list accepted or rejected with mutation");
}
void invalid(MLIRContext &ctx) {
  reject(ctx, {});
  for (SmallVector<Spec> sources : {SmallVector<Spec>{{"", "sourceA", 1}}, {{"a", "", 1}}, {{"a", "missing", 1}},
      {{"a", "sourceA", 0}}, {{"a", "sourceA", ~0U}}, {{"a", "sourceA", 1U << 25}},
      {{"a", "sourceA", 1}, {"a", "sourceB", 1}}, {{"a", "sourceA", 1}, {"b", "sourceA", 1}},
      {{"a", "sourceA", 1U << 24}, {"b", "sourceB", 1U << 24}, {"c", "sourceC", 1U << 24}},
      {{std::string("a\0bad", 5), "sourceA", 1}}, {{"a", std::string("sourceA\0bad", 11), 1}}}) reject(ctx, sources);
  reject(ctx, specs, [](CircuitOp c) { c->removeAttr("rawAnnotations"); });
  reject(ctx, specs, [](CircuitOp c) { c->setAttr("rawAnnotations", StringAttr::get(c.getContext(), "bad")); });
  reject(ctx, specs, [](CircuitOp c) { c.setName("Missing"); });
  for (auto name : {"GGCPUStreamRead", "GGCPUStreamReadWrapper"}) reject(ctx, specs, [=](CircuitOp c) {
    OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(name), ConventionAttr::get(c.getContext(), Convention::Internal), ArrayRef<PortInfo>{}); });
  for (unsigned index : {0U, 1U, 7U, 8U}) reject(ctx, specs, [=](CircuitOp c) {
    auto top = named(c, "StreamFixture"); SmallVector<bool> dirs(top.getPortDirections().begin(), top.getPortDirections().end()); dirs[index] = !dirs[index]; top.setPortDirections(dirs); });
  for (unsigned index : {0U, 1U, 7U, 8U}) reject(ctx, specs, [=](CircuitOp c) {
    auto top = named(c, "StreamFixture"); auto types = llvm::to_vector(top.getPortTypes()); auto type = UIntType::get(c.getContext(), 2, false);
    types[index] = TypeAttr::get(type); top->setAttr("portTypes", ArrayAttr::get(c.getContext(), types)); top.getArgument(index).setType(type); });
  reject(ctx, specs, [](CircuitOp c) { auto top = named(c, "StreamFixture"); auto names = llvm::to_vector(top.getPortNames()); names[9] = StringAttr::get(c.getContext(), "cpu_stream_r_valid"); top->setAttr("portNames", ArrayAttr::get(c.getContext(), names)); });
  reject(ctx, specs, [](CircuitOp c) { auto top = named(c, "StreamFixture"); OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end());
    auto owner = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Owner"), top.getConventionAttr(), ArrayRef<PortInfo>{}); b.setInsertionPointToStart(owner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); });
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); auto root = fixture(ctx);
    auto c = *root->getOps<CircuitOp>().begin(); std::string error;
    require(succeeded(goldengate::addCPUStreamRead(c, specs, error)), error); require(succeeded(verify(*root)), "generated multistream IR invalid");
    mapping(c); behavior(c);
    if (argc > 1) {
      // Reachable helper-only IR permits direct RTL comparison without any
      // synthetic fixture annotation or source producer behavior in the way.
      OwningOpRef<ModuleOp> projected(ModuleOp::create(c.getLoc())); OpBuilder b(&ctx);
      b.setInsertionPointToEnd(projected->getBody()); auto circuit = b.create<CircuitOp>(c.getLoc(), b.getStringAttr("GGCPUStreamRead"));
      b.setInsertionPointToEnd(circuit.getBodyBlock()); IRMapping map; b.clone(*named(c, "GGCPUStreamRead"), map);
      require(succeeded(verify(*projected)), "projected helper IR invalid");
      std::error_code ec; llvm::raw_fd_ostream out(argv[1], ec); require(!ec, "cannot write fixture"); projected->print(out); out << '\n';
    }
    auto before = dump(*root); require(failed(goldengate::addCPUStreamRead(c, specs, error)) && dump(*root) == before, "repeated API call mutated circuit");
    auto single = fixture(ctx); auto one = *single->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addCPUStreamRead(one, {{"single", "sourceA", 1}}, error)), error);
    require(succeeded(verify(*single)), "single minimal-depth IR invalid");
    auto helper = named(one, "GGCPUStreamRead"); require(helper.getPortName(2) == "stream" &&
        helper->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == 6 &&
        (*helper.getOps<RegResetOp>().begin()).getName() == "readBeatCounter", "single-source minimum-window compatibility differs");
    for (auto [depth, bits] : {std::pair<unsigned, unsigned>{3, 8}, {8, 9}, {9, 10}}) {
      auto varied = fixture(ctx); auto v = *varied->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::addCPUStreamRead(v, {{"a", "sourceA", 1}, {"b", "sourceB", depth}}, error)), error);
      require(succeeded(verify(*varied)), "varied-window IR invalid");
      require(named(v, "GGCPUStreamRead")->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == bits,
          "max depth window ceilLog2 rounded incorrectly");
    }
    invalid(ctx); llvm::outs() << "PASS ordered source windows, global counters, live payloads, 27 atomic rejects and repeated call\n";
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  return 0;
}
