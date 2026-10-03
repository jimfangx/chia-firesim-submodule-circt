// See LICENSE for license details.
#include "goldengate/FASEDReadLatency.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <array>
#include <deque>
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDTimingCycleWrapper\" { firrtl.module @GGFASEDTimingCycleWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=u(1);
  auto payload=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fields)es.push_back({b.getStringAttr(n),false,u(w)});return BundleType::get(&ctx,es);};
  auto dec=[&](BundleType bits){return BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},
    {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});};
  auto addr=payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests=BundleType::get(&ctx,{{b.getStringAttr("aw"),false,dec(addr)},{b.getStringAttr("w"),false,dec(data)},{b.getStringAttr("ar"),false,dec(addr)}});
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("fased_model_reset"),bit,Direction::Out},{b.getStringAttr("fased_tfire"),bit,Direction::Out},
    {b.getStringAttr("fased_timing_cycle"),u(64),Direction::Out},{b.getStringAttr("fased_read_release_cycle"),u(64),Direction::Out},
    {b.getStringAttr("fased_next_read"),dec(payload({{"id",4},{"len",8}})),Direction::In},
    {b.getStringAttr("fased_timing_requests"),requests,Direction::Out},{b.getStringAttr("other"),u(8),Direction::In}};
  if(mode>=1&&mode<=7)ps[mode-1].name=b.getStringAttr("missing");
  if(mode==8)ps[4].type=u(63);if(mode==9)ps[5].direction=Direction::Out;
  if(mode==10)ps[6].type=bit;
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  if(mode!=11){auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=17)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({
      b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(mode==16?32:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));}
  if(mode==13||mode==14)b.create<FModuleOp>(loc,b.getStringAttr(mode==13?"GGFASEDReadLatency10":"GGFASEDReadLatencyWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==15){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==18)c.setName("wrong");
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDTimingCycleWrapper"),
      b.getStringAttr("~GGFASEDTimingCycleWrapper|GGFASEDTimingCycleWrapper>other"),
      b.getStringAttr("~GGFASEDTimingCycleWrapper|GGFASEDTimingCycleWrapper>fased_next_read.bits.id"),
      b.getStringAttr("~GGFASEDTimingCycleWrapper|GGFASEDTimingCycleWrapper>fased_read_release_cycle"),
      b.getStringAttr("~GGFASEDTimingCycleWrapper|GGFASEDTokenEngine>state")}))})}));
  return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  MemOp ram;
  std::array<APInt, 10> memory;
  Interpreter(FModuleOp m) : module(m) {
    auto link = [&](Value d, Value z) {
      if (!isa<BundleType>(d.getType())) { require(drivers.emplace(key(d), z).second, "multiple drivers"); return; }
      std::function<void(std::string,std::string,Type,bool)> expand = [&](std::string dest, std::string src, Type t, bool flip) {
        if (auto bundle = dyn_cast<BundleType>(t)) {
          for (auto e : bundle.getElements()) expand(dest + "." + e.name.str(), src + "." + e.name.str(), e.type, flip != e.isFlip);
        } else require(links.emplace(flip ? src : dest, flip ? dest : src).second, "multiple aggregate drivers");
      };
      expand(key(d), key(z), d.getType(), false);
    };
    for (auto c : m.getOps<StrictConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto c : m.getOps<ConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto mem : m.getOps<MemOp>()) { require(!ram, "multiple memories"); ram = mem; }
    for (auto &word : memory) word = APInt(76, 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt read(std::string k, unsigned w) {
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    if (drivers.count(k)) return eval(drivers.at(k)).zextOrTrunc(w);
    if (links.count(k)) return read(links.at(k), w);
    auto prefix = k;
    while (prefix.find('.') != std::string::npos) {
      prefix.resize(prefix.rfind('.'));
      if (drivers.count(prefix)) return read(key(drivers.at(prefix)) + k.substr(prefix.size()), w);
    }
    throw std::runtime_error("missing aggregate driver: " + k);
  }
  APInt output(Value v, llvm::StringRef path, unsigned w = 64) {
    return read(key(v) + (path.empty() ? "" : "." + path.str()), w);
  }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = width(v);
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (ram && k == key(ram.getResult(0)) + ".data") {
      auto addr = read(key(ram.getResult(0)) + ".addr", 4).getZExtValue();
      require(addr < memory.size(), "RAM read address"); n = memory[addr];
    }
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    if (ram && !read(key(ram.getResult(1)) + ".en", 1).isZero()) {
      auto addr = read(key(ram.getResult(1)) + ".addr", 4).getZExtValue();
      require(addr < memory.size(), "RAM write address");
      require(!read(key(ram.getResult(1)) + ".mask", 1).isZero(), "RAM mask");
      memory[addr] = read(key(ram.getResult(1)) + ".data", 76);
    }
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDReadLatency(c,error)),error);require(succeeded(verify(*root)),"invalid read latency IR");
  Interpreter sim(named(c,"GGFASEDReadLatency10"));auto assertion=*sim.module.getOps<AssertOp>().begin();
  require(sim.ram.getDepth()==10 && sim.ram.getReadLatency()==0 && sim.ram.getWriteLatency()==1,"RAM geometry differs");
  std::map<std::string,Value> regs;for(auto r:sim.module.getOps<RegResetOp>())regs[r.getName().str()]=r.getResult();
  require(regs.size()==3,"queue state differs");
  struct Entry { uint64_t deadline; unsigned id,len; };std::deque<Entry> queue;
  std::mt19937_64 random(221);unsigned cases=0,flows=0,fullPops=0,stalls=0,stalledResets=0,wraps=0,resetWrites=0;
  unsigned ep=0,dp=0;bool mf=false;
  auto sample=[&](bool reset,bool fire,uint64_t cycle,bool ev,bool ready,Entry incoming) {
    bool empty=queue.empty(),full=queue.size()==10;auto head=empty?incoming:queue.front();bool due=head.deadline<=cycle;
    bool valid=(!empty||ev)&&due,push=!full&&ev&&!(empty&&ready&&due),pop=!empty&&valid&&ready;
    sim.memo.clear();sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(3),"",cycle);
    sim.put(sim.arg(4),"valid",ev);sim.put(sim.arg(4),"bits.releaseCycle",incoming.deadline);
    sim.put(sim.arg(4),"bits.id",incoming.id);sim.put(sim.arg(4),"bits.len",incoming.len);sim.put(sim.arg(5),"ready",ready);
    require(sim.output(sim.arg(4),"ready",1).getZExtValue()==!full,"queue capacity differs");
    require(sim.output(sim.arg(5),"valid",1).getZExtValue()==valid,"deadline/flow valid differs");
    require(sim.output(sim.arg(5),"bits.id",4).getZExtValue()==head.id && sim.output(sim.arg(5),"bits.len",8).getZExtValue()==head.len,"head metadata differs");
    require(sim.eval(assertion.getEnable()).getZExtValue()==(fire&&!reset),"assertion enable differs");
    require(sim.eval(assertion.getPredicate()).getZExtValue()==(!full||!ev),"overflow predicate differs");
    require(sim.read(sim.key(sim.ram.getResult(1))+".en",1).getZExtValue()==(fire&&push),"enabled RAM write differs");
    auto memory=sim.memory;if(fire&&push)memory[ep]=APInt(64,incoming.deadline).concat(APInt(4,incoming.id)).concat(APInt(8,incoming.len));
    sim.edge();require(memory==sim.memory,"RAM payload/reset/stall differs");
    flows+=fire&&empty&&ev&&ready&&due;fullPops+=fire&&full&&pop;stalls+=!fire;stalledResets+=reset&&!fire;
    resetWrites+=reset&&fire&&push;wraps+=fire&&!reset&&((push&&ep==9)||(pop&&dp==9));
    if(fire){if(reset){ep=dp=0;mf=false;queue.clear();}else{
      if(pop){queue.pop_front();dp=(dp+1)%10;}if(push){queue.push_back(incoming);ep=(ep+1)%10;}if(push!=pop)mf=push;}}
    require(sim.state.lookup(regs.at("enq_ptr_value")).getZExtValue()==ep && sim.state.lookup(regs.at("deq_ptr_value")).getZExtValue()==dp && sim.state.lookup(regs.at("maybe_full")).getZExtValue()==mf,"enabled pointers/full/reset differ");++cases;
  };
  sample(true,true,0,false,false,{0,0,0});
  sample(false,true,0,true,true,{0,3,255});sample(false,true,0,true,true,{UINT64_MAX,15,128});
  sample(true,false,UINT64_MAX,true,true,{0,1,2});sample(true,true,0,true,false,{1,2,3});
  for(unsigned round=0;round<4;++round){sample(true,true,0,false,false,{0,0,0});
    for(unsigned i=0;i<10;++i)sample(false,true,0,true,false,{i,i%16,i*17});
    sample(false,true,UINT64_MAX,true,true,{0,1,1});
    for(unsigned i=0;i<10;++i)sample(false,true,UINT64_MAX,false,true,{0,0,0});}
  for(unsigned i=0;i<30000;++i){uint64_t cycle=i%17==0?UINT64_MAX:i;
    uint64_t deadline=i%11==0?UINT64_MAX:i%13==0?0:cycle+(random()%8)-3;
    sample(i%997==0||(random()%113==0),random()%3!=0,cycle,random()%4!=0,random()%2,{deadline,unsigned(random()%16),unsigned(random()%256)});}
  require(flows&&fullPops&&stalls&&stalledResets&&wraps&&resetWrites,"queue coverage missing");
  llvm::outs()<<cases<<" read latency transitions passed; flow="<<flows<<", full pops="<<fullPops<<", stalls="<<stalls<<", stalled resets="<<stalledResets<<", wraps="<<wraps<<", reset writes="<<resetWrites<<"\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDReadLatency(c,error)),error);
  auto top=named(c,"GGFASEDReadLatencyWrapper"),inner=named(c,"GGFASEDTimingCycleWrapper");require(top.getNumPorts()==6,"wrapper port count differs");
  const unsigned copied[]{0,1,2,3,6,7};for(unsigned i=0;i<6;++i)require(top.getPorts()[i].name==inner.getPorts()[copied[i]].name&&top.getPorts()[i].type==inner.getPorts()[copied[i]].type&&top.getPorts()[i].direction==inner.getPorts()[copied[i]].direction,"copied boundary differs");
  auto it=top.getOps<InstanceOp>().begin();auto sim=*it++;auto latency=*it;Interpreter w(top);
  for(unsigned flags=0;flags<16;++flags){w.memo.clear();w.put(sim.getResult(1),"",flags&1);w.put(sim.getResult(2),"",(flags>>1)&1);
    w.put(sim.getResult(6),"ar.valid",(flags>>2)&1);w.put(w.arg(4),"ar.ready",flags>>3);
    w.put(sim.getResult(3),"",UINT64_MAX);w.put(sim.getResult(4),"",123);
    w.put(sim.getResult(6),"ar.bits.id",15);w.put(sim.getResult(6),"ar.bits.len",255);
    require(w.output(latency.getResult(1),"",1).getZExtValue()==(flags&1)&&w.output(latency.getResult(2),"",1).getZExtValue()==((flags>>1)&1),"reset/fire binding differs");
    require(w.output(latency.getResult(3),"").getZExtValue()==UINT64_MAX && w.output(latency.getResult(4),"bits.releaseCycle").getZExtValue()==123,"cycle/deadline binding differs");
    require(w.output(latency.getResult(4),"valid",1).getZExtValue()==(((flags>>2)&1)&&(flags>>3)),"AR acceptance binding differs");
    require(w.output(latency.getResult(4),"bits.id",4).getZExtValue()==15 && w.output(latency.getResult(4),"bits.len",8).getZExtValue()==255,"AR metadata binding differs");
    w.put(latency.getResult(5),"valid",1);w.put(latency.getResult(5),"bits.id",7);w.put(latency.getResult(5),"bits.len",13);w.put(sim.getResult(5),"ready",1);
    require(w.output(sim.getResult(5),"valid",1).getZExtValue()==1 && w.output(sim.getResult(5),"bits.id",4).getZExtValue()==7 && w.output(sim.getResult(5),"bits.len",8).getZExtValue()==13 && w.output(latency.getResult(5),"ready",1).getZExtValue()==1,"completion binding differs");}
  auto ts=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
  const llvm::StringRef expected[]{"~GGFASEDReadLatencyWrapper","~GGFASEDReadLatencyWrapper|GGFASEDReadLatencyWrapper>other","~GGFASEDReadLatencyWrapper|GGFASEDTimingCycleWrapper>fased_next_read.bits.id","~GGFASEDReadLatencyWrapper|GGFASEDTimingCycleWrapper>fased_read_release_cycle","~GGFASEDReadLatencyWrapper|GGFASEDTokenEngine>state"};
  for(unsigned i=0;i<5;++i)require(cast<StringAttr>(ts[i]).getValue()==expected[i],"target transfer differs");llvm::outs()<<"16 wrapper mapping cases and five target transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=18;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDReadLatency(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDReadLatency(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDReadLatency(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"19 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
