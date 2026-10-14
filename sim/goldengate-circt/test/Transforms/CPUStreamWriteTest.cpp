// See LICENSE for license details.
#include "goldengate/CPUStreamWrite.h"
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
const SmallVector<Spec> specs{{"nic", "sinkB", 3072}, {"second", "sinkA", 1}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "StreamFixture" {
    firrtl.module @StreamFixture(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      in %validA: !firrtl.uint<1>, in %dataA: !firrtl.uint<512>,
      in %validB: !firrtl.uint<1>, in %dataB: !firrtl.uint<512>, in %payload: !firrtl.uint<8>,
      in %sinkA: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      in %sinkB: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
      out %other: !firrtl.uint<8>) {}
  } })", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "StreamFixture");
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  for (unsigned i = 0; i < 2; ++i) {
    Value stream = top.getArgument(7 + i);
    b.create<StrictConnectOp>(top.getLoc(), b.create<SubfieldOp>(top.getLoc(), stream, "ready"), top.getArgument(2 + 2 * i));
  }
  b.create<StrictConnectOp>(top.getLoc(), top.getArgument(9), top.getArgument(6));
  SmallVector<Attribute> annos;
  for (auto target : {"~StreamFixture", "~StreamFixture|StreamFixture>sinkA.bits", "~StreamFixture|StreamFixture>sinkB.valid",
       "~StreamFixture|StreamFixture>other", "~StreamFixture|StreamFixture>hostReset", "~StreamFixture|StreamFixture/leaf:Leaf>foo", "~Foreign|Foreign>sinkA"})
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
    require(regs.size() == 2 && regs[0].getName() == "writeBeatCounter_0" && regs[1].getName() == "writeBeatCounter_1", "independent per-stream counters missing");
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
  Interpreter sim(named(c, "GGCPUStreamWrite")); std::mt19937_64 rng(0x173);
  unsigned cases = 0, fires = 0, responses = 0, misses = 0;
  auto cycle = [&](std::array<unsigned, 2> old, unsigned len, uint64_t addr, unsigned flags, unsigned size, uint64_t strb) {
    bool reset = flags & 1, aw = flags & 2, w = flags & 4, b = flags & 8;
    bool ready[]{bool(flags & 16), bool(flags & 32)};
    sim.memo.clear(); for (unsigned i = 0; i < 2; ++i) sim.state[sim.regs[i].getResult()] = old[i];
    sim.put(1, reset); sim.put(5, aw); sim.put(6, rng() & 65535); sim.put(7, addr);
    sim.put(8, len); sim.put(9, size); sim.put(11, w); sim.put(13, strb); sim.put(14, b);
    APInt data(512, 0); for (unsigned word = 0; word < 8; ++word) data |= APInt(512, rng()).shl(64 * word);
    sim.memo[sim.key(sim.arg(12))] = data;
    for (unsigned i = 0; i < 2; ++i) sim.memo[sim.key(sim.arg(2 + i)) + ".ready"] = APInt(1, ready[i]);
    uint64_t selector = addr >> 18; bool selected = selector < 2;
    bool space = selected && ready[selector], last = selected && old[selector] == len;
    bool fire = selected && aw && w && b && space;
    require(sim.output(4).getBoolValue() == (space && w && b && last), "AW completion predicate differs");
    require(sim.output(10).getBoolValue() == (space && aw && b), "W ready predicate differs");
    require(sim.output(15).getBoolValue() == (space && aw && w && last), "B valid must exclude its own ready");
    require(sim.output(16) == sim.eval(sim.arg(6)) && sim.output(17).isZero(), "live AW ID/OKAY differs");
    for (unsigned i = 0; i < 2; ++i) {
      require(sim.eval(sim.drivers.at(sim.key(sim.arg(2 + i)) + ".valid")).getBoolValue() == (selector == i && aw && w && b), "sink valid must exclude its own ready");
      require(sim.eval(sim.drivers.at(sim.key(sim.arg(2 + i)) + ".bits")) == data, "all sink data must be live W data");
    }
    unsigned assertions = 0;
    for (auto a : sim.module.getOps<AssertOp>()) {
      bool permitted = assertions == 0 ? !aw || size == 6 : !w || strb == ~uint64_t(0);
      require(sim.eval(a.getEnable()).getBoolValue() == !reset && sim.eval(a.getPredicate()).getBoolValue() == permitted, "valid-qualified assertion/reset gating differs"); ++assertions;
    }
    require(assertions == 2, "write size/strobe assertions missing");
    sim.edge();
    for (unsigned i = 0; i < 2; ++i) {
      unsigned expected = reset ? 0 : fire && selector == i ? (old[i] == len ? 0 : (old[i] + 1) & 511) : old[i];
      require(sim.state.lookup(sim.regs[i].getResult()) == expected, "unselected or stalled sink counter changed");
    }
    ++cases; fires += fire; responses += fire && last; misses += !selected; return fire;
  };
  for (unsigned seed = 0; seed < 512; ++seed) for (unsigned len : {0U, 1U, 127U, 255U})
    for (unsigned flags = 0; flags < 64; ++flags)
      cycle({seed, (seed + 173) & 511}, len, (uint64_t(flags % 3) << 18) | ((1U << 18) - 1), flags, flags & 7, ~uint64_t(0));
  for (unsigned lane = 0; lane < 64; ++lane) cycle({0,0},0,0,62,6,~(uint64_t(1)<<lane));
  cycle({511, 256}, 255, ~uint64_t(0), 62, 6, ~uint64_t(0));
  // Hold AW throughout each complete burst; vary FIFO and response stalls.
  for (unsigned len = 0; len < 256; ++len) {
    std::array<unsigned, 2> counts{0, 0}; unsigned sent = 0;
    while (sent <= len) {
      unsigned flags = 2 | (rng() & 1 ? 4 : 0) | (rng() & 1 ? 8 : 0) | (rng() & 1 ? 16 : 0) | (rng() & 1 ? 32 : 0);
      bool fire = cycle(counts, len, (uint64_t(len & 1) << 18) | (rng() & ((1U << 18) - 1)), flags, 6, ~uint64_t(0));
      sent += fire; counts = {sim.state.lookup(sim.regs[0].getResult()), sim.state.lookup(sim.regs[1].getResult())};
      require(counts[len & 1] == (sent <= len ? sent : 0) && counts[1 - (len & 1)] == 0, "held-AW selected burst completion differs");
    }
  }
  require(responses && misses, "last-beat/miss coverage missing");
  llvm::outs() << "Multi-stream write: " << cases << " SSA cases, " << fires << " W transfers, " << responses << " completions\n";
}
void mapping(CircuitOp c) {
  auto helper = named(c, "GGCPUStreamWrite"), wrapper = named(c, "GGCPUStreamWriteWrapper"), inner = named(c, "StreamFixture");
  require(c.getName() == wrapper.getName() && helper.getNumPorts() == 18 && wrapper.getNumPorts() == 22, "multi-stream interface shape differs");
  require(helper.getPortName(2) == "stream_0" && helper.getPortName(3) == "stream_1", "sink index naming differs");
  for (auto module : {helper}) {
    auto addressBits = module->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits");
    require(addressBits && addressBits.getInt() == 18, "common address window width differs");
    auto sources = module->getAttrOfType<ArrayAttr>("goldengate.sinkStreams"); require(sources && sources.size() == 2, "ordered sink collateral missing");
    for (auto [i, spec] : llvm::enumerate(specs)) { auto source = cast<DictionaryAttr>(sources[i]);
      require(source.getAs<StringAttr>("name").getValue() == spec.streamName && source.getAs<StringAttr>("port").getValue() == spec.portName &&
          source.getAs<IntegerAttr>("index").getInt() == i && source.getAs<IntegerAttr>("depth").getInt() == spec.depth &&
          source.getAs<IntegerAttr>("widthBytes").getInt() == 64 && source.getAs<IntegerAttr>("bufferBaseAddress").getInt() == (i << 18), "stream window metadata/order differs");
    }
  }
  InstanceOp sim, transport;
  for (auto i : wrapper.getOps<InstanceOp>()) if (i.getModuleName() == helper.getName()) transport = i; else sim = i;
  require(sim && transport, "missing wrapper instances"); llvm::DenseMap<Value, Value> drivers;
  for (auto con : wrapper.getOps<ConnectOp>()) drivers[con.getDest()] = con.getSrc();
  for (auto con : wrapper.getOps<StrictConnectOp>()) drivers[con.getDest()] = con.getSrc();
  require(drivers.lookup(sim.getResult(8)) == transport.getResult(2) && drivers.lookup(sim.getResult(7)) == transport.getResult(3), "sink wiring ignored supplied order");
  for (unsigned i = 0; i < 8; ++i) { unsigned original = i < 7 ? i : 9;
    require(wrapper.getPortName(i) == inner.getPortName(original) && wrapper.getPortType(i) == inner.getPortType(original), "retained port changed");
  }
  for (unsigned i = 4; i < 18; ++i) {
    unsigned external = i + 4;
    require(wrapper.getPortName(external) == "cpu_stream_" + helper.getPortName(i).str(), "AXI scalar port order differs");
    require(helper.getPortDirection(i) == Direction::In ? drivers.lookup(transport.getResult(i)) == wrapper.getArgument(external) : drivers.lookup(wrapper.getArgument(external)) == transport.getResult(i), "AXI port disconnected");
  }
  const char *expected[]{"~GGCPUStreamWriteWrapper", "~GGCPUStreamWriteWrapper|StreamFixture>sinkA.bits", "~GGCPUStreamWriteWrapper|StreamFixture>sinkB.valid",
      "~GGCPUStreamWriteWrapper|GGCPUStreamWriteWrapper>other", "~GGCPUStreamWriteWrapper|GGCPUStreamWriteWrapper>hostReset",
      "~GGCPUStreamWriteWrapper|StreamFixture/leaf:Leaf>foo", "~Foreign|Foreign>sinkA"};
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 7, "annotation lost");
  for (unsigned i = 0; i < 7; ++i) require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue() == expected[i], "retargeted annotation identity differs");
}
void reject(MLIRContext &ctx, ArrayRef<Spec> sources, std::function<void(CircuitOp)> mutate = {}) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); if (mutate) mutate(c);
  auto before = dump(*root); std::string error;
  require(failed(goldengate::addCPUStreamWrite(c, sources, error)) && !error.empty() && dump(*root) == before, "bad sink list accepted or rejected with mutation");
}
void invalid(MLIRContext &ctx) {
  reject(ctx, {});
  for (SmallVector<Spec> sources : {SmallVector<Spec>{{"", "sinkA", 1}}, {{"a", "", 1}}, {{"a", "missing", 1}},
      {{"a", "sinkA", 0}}, {{"a", "sinkA", ~0U}}, {{"a", "sinkA", 1U << 25}},
      {{"a", "sinkA", 1}, {"a", "sinkB", 1}}, {{"a", "sinkA", 1}, {"b", "sinkA", 1}},
      {{"a", "sinkA", 1U << 24}, {"b", "sinkB", 1U << 24}, {"c", "sinkC", 1U << 24}},
      {{std::string("a\0bad", 5), "sinkA", 1}}, {{"a", std::string("sinkA\0bad", 11), 1}}}) reject(ctx, sources);
  reject(ctx, specs, [](CircuitOp c) { c->removeAttr("rawAnnotations"); });
  reject(ctx, specs, [](CircuitOp c) { c->setAttr("rawAnnotations", StringAttr::get(c.getContext(), "bad")); });
  reject(ctx, specs, [](CircuitOp c) { c.setName("Missing"); });
  for (auto name : {"GGCPUStreamWrite", "GGCPUStreamWriteWrapper"}) reject(ctx, specs, [=](CircuitOp c) {
    OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(name), ConventionAttr::get(c.getContext(), Convention::Internal), ArrayRef<PortInfo>{}); });
  for (unsigned index : {0U, 1U, 7U, 8U}) reject(ctx, specs, [=](CircuitOp c) {
    auto top = named(c, "StreamFixture"); SmallVector<bool> dirs(top.getPortDirections().begin(), top.getPortDirections().end()); dirs[index] = !dirs[index]; top.setPortDirections(dirs); });
  for (unsigned index : {0U, 1U, 7U, 8U}) reject(ctx, specs, [=](CircuitOp c) {
    auto top = named(c, "StreamFixture"); auto types = llvm::to_vector(top.getPortTypes()); auto type = UIntType::get(c.getContext(), 2, false);
    types[index] = TypeAttr::get(type); top->setAttr("portTypes", ArrayAttr::get(c.getContext(), types)); top.getArgument(index).setType(type); });
  reject(ctx, specs, [](CircuitOp c) { auto top = named(c, "StreamFixture"); auto names = llvm::to_vector(top.getPortNames()); names[9] = StringAttr::get(c.getContext(), "cpu_stream_b_valid"); top->setAttr("portNames", ArrayAttr::get(c.getContext(), names)); });
  reject(ctx, specs, [](CircuitOp c) { auto top = named(c, "StreamFixture"); OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->end());
    auto owner = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Owner"), top.getConventionAttr(), ArrayRef<PortInfo>{}); b.setInsertionPointToStart(owner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); });
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>(); auto root = fixture(ctx);
    auto c = *root->getOps<CircuitOp>().begin(); std::string error;
    require(succeeded(goldengate::addCPUStreamWrite(c, specs, error)), error); require(succeeded(verify(*root)), "generated multistream IR invalid");
    mapping(c); behavior(c);
    if (argc > 1) {
      // Reachable helper-only IR permits direct RTL comparison without any
      // synthetic fixture annotation or sink consumer behavior in the way.
      OwningOpRef<ModuleOp> projected(ModuleOp::create(c.getLoc())); OpBuilder b(&ctx);
      b.setInsertionPointToEnd(projected->getBody()); auto circuit = b.create<CircuitOp>(c.getLoc(), b.getStringAttr("GGCPUStreamWrite"));
      b.setInsertionPointToEnd(circuit.getBodyBlock()); IRMapping map; b.clone(*named(c, "GGCPUStreamWrite"), map);
      require(succeeded(verify(*projected)), "projected helper IR invalid");
      std::error_code ec; llvm::raw_fd_ostream out(argv[1], ec); require(!ec, "cannot write fixture"); projected->print(out); out << '\n';
    }
    auto before = dump(*root); require(failed(goldengate::addCPUStreamWrite(c, specs, error)) && dump(*root) == before, "repeated API call mutated circuit");
    auto single = fixture(ctx); auto one = *single->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addCPUStreamWrite(one, {{"single", "sinkA", 1}}, error)), error);
    require(succeeded(verify(*single)), "single minimal-depth IR invalid");
    auto helper = named(one, "GGCPUStreamWrite"); require(helper.getPortName(2) == "stream" &&
        helper->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == 6 &&
        (*helper.getOps<RegResetOp>().begin()).getName() == "writeBeatCounter", "single-sink minimum-window compatibility differs");
    for (auto [depth, bits] : {std::pair<unsigned, unsigned>{3, 8}, {8, 9}, {9, 10}}) {
      auto varied = fixture(ctx); auto v = *varied->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::addCPUStreamWrite(v, {{"a", "sinkA", 1}, {"b", "sinkB", depth}}, error)), error);
      require(succeeded(verify(*varied)), "varied-window IR invalid");
      require(named(v, "GGCPUStreamWrite")->getAttrOfType<IntegerAttr>("goldengate.streamAddressSpaceBits").getInt() == bits,
          "max depth window ceilLog2 rounded incorrectly");
    }
    invalid(ctx); llvm::outs() << "PASS ordered sink windows, selected counters, live payloads, atomic rejects and repeated call\n";
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  return 0;
}
