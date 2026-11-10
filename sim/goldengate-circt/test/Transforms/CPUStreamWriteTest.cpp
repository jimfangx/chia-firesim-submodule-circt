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
  FModuleOp module; SmallVector<Value> regs;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto &op : *m.getBodyBlock())
      if (isa<RegResetOp, RegOp>(op)) regs.push_back(op.getResult(0));
    require(regs.size() == 10, "independent per-stream assembly/counter state missing");
    for (auto op : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(op.getDest()), op.getSrc()).second, "multiple transport drivers");
  }
  Value arg(unsigned i) { return module.getArgument(i); }
  void put(unsigned i, uint64_t n) { memo[key(arg(i))] = APInt(*cast<UIntType>(arg(i).getType()).getWidth(), n); }
  APInt eval(Value v) {
    auto k = key(v); unsigned width = *cast<UIntType>(v.getType()).getWidth();
    if (memo.count(k)) return memo.at(k).zextOrTrunc(width);
    auto *op = v.getDefiningOp(); APInt n(width, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v).zextOrTrunc(width);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo());
    else if (auto shl = dyn_cast_or_null<ShlPrimOp>(op)) n = eval(shl.getInput()).zextOrTrunc(width).shl(shl.getAmount());
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
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
    llvm::DenseMap<Value, APInt> next;
    for (auto r : regs) {
      auto reset = r.getDefiningOp<RegResetOp>();
      next[r] = reset && !eval(reset.getResetSignal()).isZero()
          ? eval(reset.getResetValue()) : eval(drivers.at(key(r)));
    }
    state = std::move(next);
  }
};
void behavior(CircuitOp c) {
  Interpreter sim(named(c, "GGCPUStreamWrite")); std::mt19937_64 rng(0x173);
  struct Model { unsigned beats = 0; uint64_t mask = 0, line = 0; APInt data{512, 0}; bool error = false; };
  Model models[2]; unsigned cycles=0, outputs=0, partials=0, errors=0, stalls=0;
  auto cycle = [&](bool reset, unsigned len, uint64_t addr, bool aw, bool w,
                   bool b, bool ready0, bool ready1, unsigned size, uint64_t strb) {
    sim.memo.clear(); sim.put(1,reset); sim.put(5,aw);sim.put(6,0x731);sim.put(7,addr);
    sim.put(8,len);sim.put(9,size);sim.put(11,w);sim.put(13,strb);sim.put(14,b);
    APInt data(512,0);for(unsigned word=0;word<8;++word)data|=APInt(512,rng()).shl(64*word);
    sim.memo[sim.key(sim.arg(12))]=data;
    bool ready[]{ready0,ready1};
    for(unsigned i=0;i<2;++i)sim.memo[sim.key(sim.arg(2+i))+".ready"]=APInt(1,ready[i]);
    unsigned selector=addr>>18;bool selected=selector<2;
    bool expectedAW=false,expectedW=false,expectedB=false,errorNow=false,complete=false,fire=false,malformed=false;
    uint64_t mergedMask=0,currentLine=0;APInt merged(512,0);
    if(selected){auto &m=models[selector];
      currentLine=(addr+(uint64_t(m.beats)<<size))>>6;
      malformed=(m.mask&strb)|| (m.mask&&m.line!=currentLine)||size>6||!strb||((addr+(uint64_t(m.beats)<<size))>>18)!=selector;
      errorNow=m.error||malformed;mergedMask=m.mask|strb;complete=mergedMask==~uint64_t(0);
      for(unsigned lane=0;lane<64;++lane){auto value=(strb>>lane)&1?data.extractBits(8,8*lane):m.data.extractBits(8,8*lane);merged|=value.zext(512).shl(8*lane);}
      bool accept=errorNow||!complete||ready[selector],last=m.beats==len;
      expectedAW=w&&b&&accept&&last;expectedW=aw&&b&&accept;expectedB=aw&&w&&accept&&last;
      fire=expectedW&&w;
    }
    require(sim.output(4).getBoolValue()==expectedAW,"AW completion predicate differs");
    require(sim.output(10).getBoolValue()==expectedW,"physical W readiness differs");
    require(sim.output(15).getBoolValue()==expectedB,"physical B completion differs");
    require(sim.output(16).getZExtValue()==0x731&&sim.output(17).getZExtValue()==(errorNow?2:0),"physical response ID/error differs");
    for(unsigned i=0;i<2;++i){bool valid=selected&&selector==i&&aw&&w&&b&&complete&&!errorNow;
      require(sim.eval(sim.drivers.at(sim.key(sim.arg(2+i))+".valid")).getBoolValue()==valid,"logical stream word duplicated/lost");
      if(valid){require(sim.eval(sim.drivers.at(sim.key(sim.arg(2+i))+".bits"))==merged,"assembled live bytes differ");outputs+=ready[i]&&!reset;}
    }
    unsigned assertions=0;for(auto assertion:sim.module.getOps<AssertOp>()){
      bool violation=selected&&selector==assertions&&aw&&w&&malformed;
      require(sim.eval(assertion.getEnable()).getBoolValue()==!reset&&sim.eval(assertion.getPredicate()).getBoolValue()==!violation,"partial-write assertion differs");++assertions;}
    require(assertions==2,"per-sink malformed guards missing");
    sim.edge();
    if(reset){models[0]=Model();models[1]=Model();}
    else if(fire){auto &m=models[selector];bool last=m.beats==len;m.beats=last?0:m.beats+1;m.error=last?false:errorNow;
      if(!errorNow){m.mask=complete?0:mergedMask;m.line=currentLine;m.data=merged;partials+=!complete;}else ++errors;}
    stalls+=aw&&w&&!fire;++cycles;return fire;
  };
  auto full=[&](unsigned len,uint64_t addr,bool aw,bool w,bool b,bool r0,bool r1){return cycle(false,len,addr,aw,w,b,r0,r1,6,~uint64_t(0));};
  cycle(true,0,0,false,false,false,false,false,6,~uint64_t(0));
  // Full DMA beats preserve selected burst counters through independent stalls.
  for(unsigned sink=0;sink<2;++sink)for(unsigned len:{0u,1u,31u,127u,255u}){
    unsigned done=0;while(done<=len){bool fire=full(len,uint64_t(sink)<<18,rng()%4,rng()%4,rng()%4,rng()%4,rng()%4);done+=fire;}
  }
  // Interleave every byte of two independent partial lines. Deliberately
  // refuse FIFO readiness until the final byte; partial physical B must retire.
  for(unsigned repetition=0;repetition<8;++repetition){
    for(unsigned lane=0;lane<64;++lane)for(unsigned sink=0;sink<2;++sink){
      uint64_t addr=(uint64_t(sink)<<18)+repetition*64+lane;
      if(lane==63){require(!cycle(false,0,addr,true,true,true,false,false,6,uint64_t(1)<<lane),"complete word ignored FIFO backpressure");}
      require(cycle(false,0,addr,true,true,true,true,true,6,uint64_t(1)<<lane),"partial physical transaction failed to retire");
    }
  }
  // A burst can start with a partial line, then finish it in a later AW;
  // effective physical beat addresses must not merge adjacent lines.
  for(unsigned sink=0;sink<2;++sink){uint64_t base=uint64_t(sink)<<18;
    cycle(false,0,base,true,true,true,true,true,6,0xffffffff);
    cycle(false,0,base+32,true,true,true,false,false,6,0xffffffff00000000ULL);
    cycle(false,0,base+32,true,true,true,true,true,6,0xffffffff00000000ULL);
    // Overlap and wrong-line fail without discarding pending owned bytes.
    cycle(false,0,base,true,true,true,true,true,6,0xff);
    cycle(false,0,base,true,true,true,true,true,6,0x0f);
    cycle(false,0,base+64,true,true,true,true,true,6,0xff00);
    cycle(false,0,base+8,true,true,true,true,true,6,~uint64_t(0xff));
    cycle(false,0,base,true,true,true,true,true,6,0);
    cycle(false,0,base,true,true,true,true,true,7,~uint64_t(0));
  }
  // Bad earlier physical beat propagates SLVERR to the final B response.
  cycle(false,1,0,true,true,true,true,true,6,0xff);
  cycle(false,1,0,true,true,true,true,true,6,0xff); // wrong effective line
  cycle(true,0,0,false,false,false,false,false,6,~uint64_t(0));
  full(0,0,true,true,true,true,true);
  require(outputs>800&&partials>900&&errors>=8&&stalls>100,"insufficient full/partial/stall/error coverage");
  llvm::outs()<<"Multi-stream write: "<<cycles<<" cycles, "<<outputs<<" logical words, "<<partials<<" partial transactions, "<<errors<<" rejected transactions\n";
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
