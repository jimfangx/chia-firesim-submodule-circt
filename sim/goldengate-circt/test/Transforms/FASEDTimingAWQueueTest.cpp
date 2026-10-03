// See LICENSE for license details.
#include "goldengate/FASEDTimingAWQueue.h"
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDWriteLatencyWrapper\" { firrtl.module @GGFASEDWriteLatencyWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=u(1);
  auto payload=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fields)es.push_back({b.getStringAttr(n),false,u(w)});return BundleType::get(&ctx,es);};
  auto dec=[&](BundleType bits){return BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},
    {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});};
  auto address = payload({{"user",1},{"id",mode==7?5u:4u},{"region",4},{"qos",4},{"prot",3},{"cache",4},
      {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto aw=dec(address);
  if(mode==8)aw=BundleType::get(&ctx,{{b.getStringAttr("ready"),false,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,address}});
  auto requests=BundleType::get(&ctx,{{b.getStringAttr("aw"),false,aw},
      {b.getStringAttr("w"),false,dec(data)},{b.getStringAttr("ar"),false,dec(address)}});
  auto completion=BundleType::get(&ctx,{{b.getStringAttr("valid"),false,bit},
      {b.getStringAttr("bits"),false,payload({{"id",mode==6?5u:4u}})}});
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("fased_model_reset"),bit,Direction::Out},{b.getStringAttr("fased_tfire"),bit,Direction::Out},
    {b.getStringAttr("fased_timing_requests"),requests,Direction::Out},
    {b.getStringAttr("fased_write_completion"),completion,Direction::In},{b.getStringAttr("other"),u(8),Direction::In}};
  if(mode>=1&&mode<=5)ps[mode-1].name=b.getStringAttr("missing");
  if(mode==9)ps[3].direction=Direction::In;if(mode==10)ps[1].type=u(2);
  if(mode==21)ps[4].direction=Direction::Out;
  if(mode==19||mode==20)ps[5].name=b.getStringAttr(mode==19?"fased_write_pair_complete":"fased_timing_aw_queue_ready");
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  if(mode!=11){auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=17)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({
      b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(mode==16?32:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));}
  if(mode==13||mode==14)b.create<FModuleOp>(loc,b.getStringAttr(mode==13?"GGFASEDTimingAWQueue10":"GGFASEDTimingAWQueueWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==15){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==18)c.setName("wrong");
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDWriteLatencyWrapper"),
      b.getStringAttr("~GGFASEDWriteLatencyWrapper|GGFASEDWriteLatencyWrapper>other"),
      b.getStringAttr("~GGFASEDWriteLatencyWrapper|GGFASEDWriteLatencyWrapper>fased_write_completion.bits.id"),
      b.getStringAttr("~GGFASEDWriteLatencyWrapper|GGFASEDWriteLatencyWrapper>fased_timing_requests.aw.bits.id"),
      b.getStringAttr("~GGFASEDWriteLatencyWrapper|GGFASEDTokenEngine>state")}))})}));
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
    for (auto &word : memory) word = APInt(4, 0);
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
      memory[addr] = read(key(ram.getResult(1)) + ".data", 4);
    }
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDTimingAWQueue(c,error)),error);require(succeeded(verify(*root)),"invalid write latency IR");
  Interpreter sim(named(c,"GGFASEDTimingAWQueue10"));auto assertion=*sim.module.getOps<AssertOp>().begin();
  require(sim.ram.getDepth()==10 && sim.ram.getReadLatency()==0 && sim.ram.getWriteLatency()==1,"RAM geometry differs");
  std::map<std::string,Value> regs;for(auto r:sim.module.getOps<RegResetOp>())regs[r.getName().str()]=r.getResult();
  require(regs.size()==3,"queue state differs");
  std::deque<unsigned> queue;
  std::mt19937_64 random(223);unsigned cases=0,flows=0,fullPops=0,stalls=0,stalledResets=0,wraps=0,resetWrites=0;
  unsigned ep=0,dp=0;bool mf=false;
  auto sample=[&](bool reset,bool fire,bool ev,bool ready,unsigned id) {
    bool empty=queue.empty(),full=queue.size()==10;unsigned head=empty?id:queue.front();
    bool valid=!empty||ev,push=!full&&ev&&!(empty&&ready),pop=!empty&&valid&&ready;
    sim.memo.clear();sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);
    sim.put(sim.arg(3),"valid",ev);sim.put(sim.arg(3),"bits.id",id);sim.put(sim.arg(4),"ready",ready);
    require(sim.output(sim.arg(3),"ready",1).getZExtValue()==!full,"queue capacity differs");
    require(sim.output(sim.arg(4),"valid",1).getZExtValue()==valid,"flow valid differs");
    require(sim.output(sim.arg(4),"bits.id",4).getZExtValue()==head,"head ID differs");
    require(sim.eval(assertion.getEnable()).getZExtValue()==(fire&&!reset),"assertion enable differs");
    require(sim.eval(assertion.getPredicate()).getZExtValue()==(!full||!ev),"overflow predicate differs");
    require(sim.read(sim.key(sim.ram.getResult(1))+".en",1).getZExtValue()==(fire&&push),"enabled RAM write differs");
    auto memory=sim.memory;if(fire&&push)memory[ep]=APInt(4,id);
    sim.edge();require(memory==sim.memory,"RAM payload/reset/stall differs");
    flows+=fire&&empty&&ev&&ready;fullPops+=fire&&full&&pop;stalls+=!fire;stalledResets+=reset&&!fire;
    resetWrites+=reset&&fire&&push;wraps+=fire&&!reset&&((push&&ep==9)||(pop&&dp==9));
    if(fire){if(reset){ep=dp=0;mf=false;queue.clear();}else{
      if(pop){queue.pop_front();dp=(dp+1)%10;}if(push){queue.push_back(id);ep=(ep+1)%10;}if(push!=pop)mf=push;}}
    require(sim.state.lookup(regs.at("enq_ptr_value")).getZExtValue()==ep && sim.state.lookup(regs.at("deq_ptr_value")).getZExtValue()==dp && sim.state.lookup(regs.at("maybe_full")).getZExtValue()==mf,"enabled pointers/full/reset differ");++cases;
  };
  sample(true,true,false,false,0);sample(false,true,true,true,15);
  sample(true,false,true,false,1);sample(true,true,true,false,2);
  for(unsigned round=0;round<4;++round){sample(true,true,false,false,0);
    for(unsigned i=0;i<10;++i)sample(false,true,true,false,i);
    sample(false,true,true,true,15);
    for(unsigned i=0;i<10;++i)sample(false,true,false,true,0);}
  for(unsigned i=0;i<30000;++i)sample(i%997==0||(random()%113==0),random()%3!=0,random()%4!=0,random()%2,random()%16);
  require(flows&&fullPops&&stalls&&stalledResets&&wraps&&resetWrites,"queue coverage missing");
  llvm::outs()<<cases<<" timing AW queue transitions passed; flow="<<flows<<", full pops="<<fullPops<<", stalls="<<stalls<<", stalled resets="<<stalledResets<<", wraps="<<wraps<<", reset writes="<<resetWrites<<"\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDTimingAWQueue(c,error)),error);
  auto top=named(c,"GGFASEDTimingAWQueueWrapper"),inner=named(c,"GGFASEDWriteLatencyWrapper");require(top.getNumPorts()==7,"wrapper port count differs");
  const unsigned copied[]{0,1,2,3,5};for(unsigned i=0;i<5;++i)require(top.getPorts()[i].name==inner.getPorts()[copied[i]].name&&top.getPorts()[i].type==inner.getPorts()[copied[i]].type&&top.getPorts()[i].direction==inner.getPorts()[copied[i]].direction,"copied boundary differs");
  auto it=top.getOps<InstanceOp>().begin();auto sim=*it++;auto q=*it;Interpreter w(top);
  for(unsigned flags=0;flags<64;++flags){w.memo.clear();
    bool reset=flags&1,fire=(flags>>1)&1,av=(flags>>2)&1,ar=(flags>>3)&1,paired=(flags>>4)&1,capacity=(flags>>5)&1;
    w.put(sim.getResult(1),"",reset);w.put(sim.getResult(2),"",fire);
    w.put(sim.getResult(3),"aw.valid",av);w.put(sim.getResult(3),"aw.ready",ar);w.put(sim.getResult(3),"aw.bits.id",15);
    w.put(w.arg(5),"",paired);w.put(q.getResult(3),"ready",capacity);w.put(q.getResult(4),"bits.id",7);
    require(w.output(q.getResult(1),"",1).getZExtValue()==reset&&w.output(q.getResult(2),"",1).getZExtValue()==fire,"reset/fire binding differs");
    require(w.output(q.getResult(3),"valid",1).getZExtValue()==(av&&ar),"AW enqueue pulse differs or is gated by capacity/fire/reset");
    require(w.output(q.getResult(3),"bits.id",4).getZExtValue()==15,"accepted AW ID binding differs");
    require(w.output(q.getResult(4),"ready",1).getZExtValue()==paired && w.output(sim.getResult(4),"valid",1).getZExtValue()==paired,"pairing pulse gated or disconnected");
    require(w.output(sim.getResult(4),"bits.id",4).getZExtValue()==7,"write completion ID not bound to oldest AW");
    require(w.output(w.arg(6),"",1).getZExtValue()==capacity,"capacity observation differs");}
  auto ts=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
  const llvm::StringRef expected[]{"~GGFASEDTimingAWQueueWrapper","~GGFASEDTimingAWQueueWrapper|GGFASEDTimingAWQueueWrapper>other","~GGFASEDTimingAWQueueWrapper|GGFASEDWriteLatencyWrapper>fased_write_completion.bits.id","~GGFASEDTimingAWQueueWrapper|GGFASEDTimingAWQueueWrapper>fased_timing_requests.aw.bits.id","~GGFASEDTimingAWQueueWrapper|GGFASEDTokenEngine>state"};
  for(unsigned i=0;i<5;++i)require(cast<StringAttr>(ts[i]).getValue()==expected[i],"target transfer differs");llvm::outs()<<"64 wrapper mapping cases and five target transfers passed\n";
}

void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=21;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDTimingAWQueue(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDTimingAWQueue(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDTimingAWQueue(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"22 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
