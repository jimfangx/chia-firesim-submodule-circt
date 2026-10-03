// See LICENSE for license details.
#include "goldengate/FASEDWritePairing.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDTimingAWQueueWrapper\" { firrtl.module @GGFASEDTimingAWQueueWrapper() {} } }",&ctx);
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
  SmallVector<PortInfo> ps{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("fased_model_reset"),bit,Direction::Out},{b.getStringAttr("fased_tfire"),bit,Direction::Out},
    {b.getStringAttr("fased_timing_requests"),requests,Direction::Out},
    {b.getStringAttr("fased_write_pair_complete"),u(mode==6?2:1),Direction::In},{b.getStringAttr("other"),u(8),Direction::In}};
  if(mode>=1&&mode<=5)ps[mode-1].name=b.getStringAttr("missing");
  if(mode==9)ps[3].direction=Direction::In;if(mode==10)ps[1].type=u(2);
  if(mode==21)ps[4].direction=Direction::Out;
  if(mode==19||mode==20)ps[5].name=b.getStringAttr(mode==19?"fased_target_b_fire":"fased_write_max_reqs");
  if(mode==22)ps[5].name=b.getStringAttr("fased_pending_writes");
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  if(mode!=11){auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=17)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({
      b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(mode==16?32:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));}
  if(mode==13||mode==14)b.create<FModuleOp>(loc,b.getStringAttr(mode==13?"GGFASEDWritePairing":"GGFASEDWritePairingWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==15){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==18)c.setName("wrong");
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDTimingAWQueueWrapper"),
      b.getStringAttr("~GGFASEDTimingAWQueueWrapper|GGFASEDTimingAWQueueWrapper>other"),
      b.getStringAttr("~GGFASEDTimingAWQueueWrapper|GGFASEDTimingAWQueueWrapper>fased_write_pair_complete"),
      b.getStringAttr("~GGFASEDTimingAWQueueWrapper|GGFASEDTimingAWQueueWrapper>fased_timing_requests.aw.bits.id"),
      b.getStringAttr("~GGFASEDTimingAWQueueWrapper|GGFASEDTokenEngine>state")}))})}));
  return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
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
    else if (isa_and_nonnull<GTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ugt(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDWritePairing(c,error)),error);require(succeeded(verify(*root)),"invalid pairing IR");
  Interpreter sim(named(c,"GGFASEDWritePairing"));SmallVector<Value> regs;
  for(auto r:sim.module.getOps<RegResetOp>())regs.push_back(r.getResult());require(regs.size()==2,"unexpected pending state");
  unsigned cases=0,stalls=0,stalledResets=0,simultaneous=0,atFull=0,underflows=0,limitBelow=0;
  auto sample=[&](unsigned aw,unsigned w,unsigned maximum,unsigned flags) {
    bool reset=flags&1,fire=flags&2,ai=flags&4,wi=flags&8,bi=flags&16;
    sim.memo.clear();sim.state[regs[0]]=APInt(4,aw);sim.state[regs[1]]=APInt(4,w);
    for(unsigned j=1;j<7;++j)sim.put(sim.arg(j),"",j==1?reset:j==2?fire:j==3?ai:j==4?wi:j==5?bi:maximum);
    bool paired=ai&&wi;
    if(w>aw&&ai)paired=true;if(aw>w&&wi)paired=true;
    require(sim.output(sim.arg(7),"",1).getZExtValue()==paired,"pre-edge pairing differs");
    require(sim.output(sim.arg(8),"awValue",4).getZExtValue()==aw&&sim.output(sim.arg(8),"wValue",4).getZExtValue()==w,"pending count observation differs");
    require(sim.output(sim.arg(8),"awFull",1).getZExtValue()==(aw>=maximum)&&sim.output(sim.arg(8),"wFull",1).getZExtValue()==(w>=maximum),"runtime full differs");
    auto next=[&](unsigned value,bool inc) {
      if(!fire)return value;if(reset)return 0u;
      if(inc==bi)return value;
      if(inc)return value<maximum?value+1:value;
      return value?value-1:0;
    };
    unsigned na=next(aw,ai),nw=next(w,wi);sim.edge();
    require(sim.state.lookup(regs[0]).getZExtValue()==na&&sim.state.lookup(regs[1]).getZExtValue()==nw,"saturation/retirement/clock/reset differs");
    ++cases;stalls+=!fire;stalledResets+=!fire&&reset;simultaneous+=fire&&!reset&&bi&&(ai||wi);
    atFull+=fire&&!reset&&((ai&&aw>=maximum)||(wi&&w>=maximum));underflows+=fire&&!reset&&bi&&(!aw||!w);
    limitBelow+=aw>maximum||w>maximum;return std::pair<unsigned,unsigned>{na,nw};
  };
  for(unsigned aw=0;aw<16;++aw)for(unsigned w=0;w<16;++w)for(unsigned max=0;max<16;++max)
    for(unsigned flags=0;flags<32;++flags)sample(aw,w,max,flags);
  std::mt19937 random(224);unsigned aw=0,w=0;
  for(unsigned i=0;i<20000;++i){auto next=sample(aw,w,random()%11,(random()&30)|(i%997==0));aw=next.first;w=next.second;}
  require(stalls&&stalledResets&&simultaneous&&atFull&&underflows&&limitBelow,"pending coverage missing");
  llvm::outs()<<cases<<" pending-counter/pairing transitions passed; stalls="<<stalls<<", stalled resets="<<stalledResets
    <<", simultaneous="<<simultaneous<<", full="<<atFull<<", empty retirements="<<underflows<<", reduced limits="<<limitBelow<<"\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDWritePairing(c,error)),error);
  auto top=named(c,"GGFASEDWritePairingWrapper"),inner=named(c,"GGFASEDTimingAWQueueWrapper");require(top.getNumPorts()==8,"wrapper port count differs");
  const unsigned copied[]{0,1,2,3,5};for(unsigned i=0;i<5;++i)require(top.getPorts()[i].name==inner.getPorts()[copied[i]].name&&top.getPorts()[i].type==inner.getPorts()[copied[i]].type&&top.getPorts()[i].direction==inner.getPorts()[copied[i]].direction,"copied boundary differs");
  auto it=top.getOps<InstanceOp>().begin();auto sim=*it++;auto q=*it;Interpreter w(top);
  for(unsigned flags=0;flags<256;++flags){w.memo.clear();
    bool reset=flags&1,fire=(flags>>1)&1,av=(flags>>2)&1,ar=(flags>>3)&1,wv=(flags>>4)&1,wr=(flags>>5)&1,last=(flags>>6)&1,bi=(flags>>7)&1;
    w.put(sim.getResult(1),"",reset);w.put(sim.getResult(2),"",fire);
    w.put(sim.getResult(3),"aw.valid",av);w.put(sim.getResult(3),"aw.ready",ar);
    w.put(sim.getResult(3),"w.valid",wv);w.put(sim.getResult(3),"w.ready",wr);w.put(sim.getResult(3),"w.bits.last",last);
    w.put(w.arg(5),"",bi);w.put(w.arg(6),"",10);w.put(q.getResult(7),"",flags&1);
    for(auto field:{"awValue","wValue","awFull","wFull"})w.put(q.getResult(8),field,flags%16);
    require(w.output(q.getResult(1),"",1).getZExtValue()==reset&&w.output(q.getResult(2),"",1).getZExtValue()==fire,"model reset/fire binding differs");
    require(w.output(q.getResult(3),"",1).getZExtValue()==(av&&ar),"AW acceptance gated or disconnected");
    require(w.output(q.getResult(4),"",1).getZExtValue()==(wv&&wr&&last),"W last acceptance gated or disconnected");
    require(w.output(q.getResult(5),"",1).getZExtValue()==bi&&w.output(q.getResult(6),"",4).getZExtValue()==10,"retirement/limit binding differs");
    require(w.output(sim.getResult(4),"",1).getZExtValue()==(flags&1),"completion pulse gated or disconnected");
    for(auto field:{"awValue","wValue","awFull","wFull"})require(w.output(w.arg(7),field,llvm::StringRef(field).ends_with("Full")?1:4).getZExtValue()==((flags%16)&(llvm::StringRef(field).ends_with("Full")?1:15)),"pending diagnostic binding differs");
  }
  auto ts=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
  const llvm::StringRef expected[]{"~GGFASEDWritePairingWrapper","~GGFASEDWritePairingWrapper|GGFASEDWritePairingWrapper>other","~GGFASEDWritePairingWrapper|GGFASEDTimingAWQueueWrapper>fased_write_pair_complete","~GGFASEDWritePairingWrapper|GGFASEDWritePairingWrapper>fased_timing_requests.aw.bits.id","~GGFASEDWritePairingWrapper|GGFASEDTokenEngine>state"};
  for(unsigned i=0;i<5;++i)require(cast<StringAttr>(ts[i]).getValue()==expected[i],"target transfer differs");llvm::outs()<<"256 wrapper mapping cases and five target transfers passed\n";
}

void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=22;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWritePairing(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDWritePairing(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWritePairing(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"23 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
