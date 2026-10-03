// See LICENSE for license details.
#include "goldengate/LoadMemWriter.h"
#include "llvm/ADT/APSInt.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) { if (!ok) throw std::runtime_error(message.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == n) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGCPUStreamWriteResponseBufferWrapper" {
      firrtl.module @GGCPUStreamWriteResponseBufferWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %cpu_stream_b_valid: !firrtl.uint<1>, in %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
      b.getNamedAttr("targets", b.getArrayAttr({
        b.getStringAttr("~GGCPUStreamWriteResponseBufferWrapper"),
        b.getStringAttr("~GGCPUStreamWriteResponseBufferWrapper|GGCPUStreamWriteResponseBufferWrapper>cpu_stream_b_valid"),
        b.getStringAttr("~GGCPUStreamWriteResponseBufferWrapper|Model>clock")}))})}));
  return root;
}
// Evaluate the actual generated SSA operations and connect drivers. Track
// unreset payload registers separately from the two reset control registers.
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value, Value> drivers;
  llvm::DenseMap<Value, uint64_t> memo, state;
  std::map<std::string, Value> ports, regs;
  Interpreter(FModuleOp m) : module(m) {
    for (auto [i, p] : llvm::enumerate(m.getPorts())) ports.emplace(p.name.str(), m.getBodyBlock()->getArgument(i));
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.try_emplace(c.getDest(), c.getSrc()).second, "multiple drivers");
    for (auto &op : m.getBodyBlock()->getOperations()) {
      if (auto r = dyn_cast<RegOp>(op)) { regs[r.getName().str()] = r.getResult(); state[r.getResult()] = 0; }
      if (auto r = dyn_cast<RegResetOp>(op)) {
        regs[r.getName().str()] = r.getResult(); state[r.getResult()] = 0;
        require(r.getClockVal() == ports.at("clock") && r.getResetSignal() == ports.at("reset"), "wrong reset/clock");
      }
    }
    require(regs.size() == 5 && std::distance(m.getOps<RegOp>().begin(), m.getOps<RegOp>().end()) == 3 &&
        std::distance(m.getOps<RegResetOp>().begin(), m.getOps<RegResetOp>().end()) == 2, "payload reset semantics differ");
  }
  uint64_t eval(Value v) {
    if (memo.count(v)) return memo.lookup(v);
    uint64_t n; auto *op = v.getDefiningOp();
    if (state.count(v)) n = state.lookup(v);
    else if (drivers.count(v)) n = eval(drivers.lookup(v));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<GTPrimOp>(op)) n = eval(op->getOperand(0)) > eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (auto shift = dyn_cast_or_null<ShlPrimOp>(op)) n = eval(shift.getInput()) << shift.getAmount();
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()) >> bits.getLo();
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else throw std::runtime_error("unsupported generated operation");
    unsigned width = *cast<UIntType>(v.getType()).getWidth();
    if (width < 64) n &= (uint64_t(1) << width) - 1;
    memo[v] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto item : regs) {
      Value reg = item.second;
      auto reset = reg.getDefiningOp<RegResetOp>();
      next[reg] = reset && eval(reset.getResetSignal()) ? eval(reset.getResetValue()) : eval(drivers.lookup(reg));
    }
    state = std::move(next);
  }
};
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemWriter(c, error)), error); require(succeeded(verify(*root)), "writer IR invalid");
  Interpreter sim(named(c, "GGLoadMemWriter")); std::mt19937_64 rng(168);
  constexpr uint64_t addressMask = (uint64_t(1) << 34) - 1;
  uint64_t address = 0, length = 0, beats = 0, zero = 0, state = 0;
  unsigned cases = 0, requests = 0, bursts = 0, writes = 0, responses = 0, resetPayload = 0;
  auto cycle = [&](unsigned reset, unsigned req, unsigned rz, uint64_t ra, uint64_t rl,
                   unsigned valid, uint64_t word, unsigned awReady, unsigned wReady, unsigned bValid) {
    sim.memo.clear();
    for (auto input : std::map<std::string, uint64_t>{{"reset",reset},{"req_valid",req},{"req_bits_zero",rz},
      {"req_bits_addr",ra},{"req_bits_len",rl},{"data_valid",valid},{"data_bits",word},
      {"mem_aw_ready",awReady},{"mem_w_ready",wReady},{"mem_b_valid",bValid}})
      sim.memo[sim.ports.at(input.first)] = input.second;
    uint64_t burst = std::min<uint64_t>(length, 32);
    std::map<std::string, uint64_t> expected{{"req_ready",state == 0},{"data_ready",state == 2 && !zero && wReady},
      {"mem_aw_valid",state == 1},{"mem_aw_bits_addr",address},{"mem_aw_bits_len",(burst - 1) & 255},
      {"mem_w_valid",state == 2 && (zero || valid)},{"mem_w_bits_data",zero ? 0 : word},
      {"mem_w_bits_last",beats == 0},{"mem_b_ready",state == 3}};
    for (auto out : expected) require(sim.eval(sim.ports.at(out.first)) == out.second, "writer output differs");
    bool capture = state == 0 && req;
    if (capture) { zero = rz; address = ra; length = rl; state = 1; ++requests; }
    else if (state == 1 && awReady) {
      beats = (burst - 1) & 31; length -= burst; address = (address + burst * 8) & addressMask;
      state = 2; ++bursts;
    } else if (state == 2 && wReady && (zero || valid)) {
      if (!beats) state = 3; beats = (beats - 1) & 31; ++writes;
    } else if (state == 3 && bValid) { state = length ? 1 : 0; ++responses; }
    if (reset) { state = beats = 0; resetPayload += capture; }
    sim.edge();
    for (auto reg : std::map<std::string, uint64_t>{{"wZero",zero},{"wAddr",address},{"wLen",length},{"wBeatsLeft",beats},{"state",state}})
      require(sim.state.lookup(sim.regs.at(reg.first)) == reg.second, "writer state differs");
    ++cases;
  };
  // Exercise all short lengths, zero fill, multi-burst writes and address wrap.
  for (unsigned z = 0; z < 2; ++z) for (unsigned len = 0; len < 97; ++len) {
    cycle(1,0,0,0,0,0,0,0,0,0);
    cycle(0,1,z,addressMask - 7,len,0,rng(),0,0,0);
    do { cycle(0,0,0,0,0,rng()%4!=0,rng(),rng()%4!=0,rng()%4!=0,rng()%4!=0); } while(state);
  }
  // Reset coincident with handshakes must reset control but retain payload updates.
  for (unsigned stage = 0; stage < 4; ++stage) for (unsigned z = 0; z < 2; ++z) {
    cycle(1,0,0,0,0,0,0,0,0,0);
    for (unsigned i = 0; i < stage; ++i)
      cycle(0,1,z,addressMask - 7,1,1,rng(),1,1,1);
    cycle(1,1,z,addressMask - 7,1,1,rng(),1,1,1);
  }
  for (unsigned i = 0; i < 30000; ++i)
    cycle(i%31==0,rng()&1,rng()&1,rng()&addressMask,rng()%97,rng()&1,rng(),rng()&1,rng()&1,rng()&1);
  llvm::outs() << "LoadMem writer: " << cases << " cycles, " << requests << " requests, " << bursts << " bursts, "
      << writes << " beats, " << responses << " responses, " << resetPayload << " payload captures during reset\n";
  require(requests > 500 && bursts > 500 && writes > 10000 && responses > 100 && resetPayload >= 2, "writer coverage insufficient");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemWriter(c,error)),error);
  auto top = named(c,"GGLoadMemWriterWrapper"), h = named(c,"GGLoadMemWriter");
  require(h.getNumPorts() == 20 && top.getNumPorts() == 22, "writer port geometry differs");
  require(h->getAttrOfType<IntegerAttr>("goldengate.maxBurst").getInt() == 32, "burst metadata differs");
  InstanceOp sim, writer;
  for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == h.getName()) writer = i; else sim = i;
  require(sim && writer,"writer instances missing");
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  auto wired = [&](Value d,Value s) { for(auto c:top.getOps<StrictConnectOp>()) if(c.getDest()==d && c.getSrc()==s) return true; return false; };
  require(wired(writer.getResult(0),arg(0)) && wired(writer.getResult(1),arg(1)),"writer clock/reset disconnected");
  for(unsigned i=2;i<20;++i) {
    require(top.getPortName(i+2)=="loadmem_"+h.getPortName(i).str() && top.getPortType(i+2)==h.getPortType(i) &&
        top.getPortDirection(i+2)==h.getPortDirection(i),"writer scalar port differs");
    require(h.getPortDirection(i)==Direction::In ? wired(writer.getResult(i),arg(i+2)) : wired(arg(i+2),writer.getResult(i)),"writer port disconnected");
  }
  unsigned copies=0;for(auto conn:top.getOps<ConnectOp>()) {
    unsigned i=copies++;bool input=i!=2;
    require(conn.getDest()==(input?sim.getResult(i):arg(i)) && conn.getSrc()==(input?arg(i):sim.getResult(i)),"copied port disconnected");
  }
  require(copies==4,"copied ports lost");
  auto a=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  auto t=a.getAs<ArrayAttr>("targets");
  require(cast<StringAttr>(t[0]).getValue()=="~GGLoadMemWriterWrapper" &&
      cast<StringAttr>(t[1]).getValue()=="~GGLoadMemWriterWrapper|GGLoadMemWriterWrapper>cpu_stream_b_valid" &&
      cast<StringAttr>(t[2]).getValue()=="~GGLoadMemWriterWrapper|Model>clock","annotation identity differs");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<9;++bad) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGCPUStreamWriteResponseBufferWrapper");OpBuilder b(&context);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2 || bad==7) { SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());
      names[bad==2?0:3]=b.getStringAttr(bad==2?"WrongClock":"loadmem_req_valid");top.setPortNames(names); }
    if(bad==3) { SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[1]=TypeAttr::get(UIntType::get(&context,8,false));top.setPortTypes(types); }
    if(bad==4) { b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used"); }
    if(bad==5 || bad==6) { b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==5?"GGLoadMemWriter":"GGLoadMemWriterWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
    if(bad==8)top.setName("MissingTop");
    std::string before,after,error;{llvm::raw_string_ostream o(before);root->print(o);}
    require(failed(goldengate::addLoadMemWriter(c,error)),"unsupported writer boundary accepted");
    {llvm::raw_string_ostream o(after);root->print(o);}require(before==after,"rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext c;c.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(c);mapping(c);rejection(c);return 0; }
  catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n';return 1; }
}
