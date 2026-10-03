// See LICENSE for license details.
#include "goldengate/FASEDIngressOrder.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDIngressCreditsWrapper\" { firrtl.module @GGFASEDIngressCreditsWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();(*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=uint(1);
  auto status=BundleType::get(&ctx,{{b.getStringAttr("awValue"),false,uint(4)},{b.getStringAttr("wValue"),false,uint(4)},{b.getStringAttr("awEmpty"),false,bit},{b.getStringAttr("wEmpty"),false,bit},{b.getStringAttr("writeReqDone"),mode==8,bit}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},{b.getStringAttr("fased_ingress_reset"),bit,Direction::Out},{b.getStringAttr("fased_ingress_ar_enq_fire"),bit,Direction::Out},{b.getStringAttr("fased_ingress_credits"),status,Direction::Out},{b.getStringAttr("other"),uint(8),Direction::Out}};
  if(mode>=1&&mode<=4)ports[mode-1].name=b.getStringAttr("missing");
  if(mode==5)ports[0].type=bit;if(mode==6)ports[2].type=uint(2);if(mode==7)ports[1].direction=Direction::In;
  if(mode==9||mode==10)ports[4].name=b.getStringAttr(mode==9?"fased_ingress_order":"fased_ingress_order_ready");
  auto top=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  if(mode!=11){auto engine=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=12)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(mode==13?8:10))}))}));}
  if(mode==14||mode==15)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==14?"GGFASEDIngressOrder20":"GGFASEDIngressOrderWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==16){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}if(mode==17)c.setName("other");
  SmallVector<Attribute> raw;for(auto suffix:{"","|GGFASEDIngressCreditsWrapper>other","|GGFASEDIngressCreditsWrapper>fased_ingress_credits.writeReqDone","|GGFASEDTokenEngine"})raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr(std::string("~GGFASEDIngressCreditsWrapper")+suffix))}));
  if(mode!=18)c->setAttr("rawAnnotations",b.getArrayAttr(raw));return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, MemOp> rams;
  std::map<std::string, std::array<APInt,10>> memory;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    for(auto ram:m.getOps<MemOp>()) {
      require(ram.getDepth()==10 && ram.getDataType()==UIntType::get(m.getContext(),1,false) && ram.getReadLatency()==0 && ram.getWriteLatency()==1 && ram.getRuw()==RUWAttr::Undefined && ram.getNumResults()==2 && ram.getPortKind(size_t(0))==MemOp::PortKind::Read && ram.getPortKind(size_t(1))==MemOp::PortKind::Write,"bank RAM geometry differs");
      auto name=key(ram.getResult(0));rams.emplace(name,ram);
      for(unsigned j=0;j<10;++j)memory[name][j]=APInt(1,j&1);
    }
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
    for(auto [name,ram]:rams)if(k==name+".data") {
      require(read(name+".en",1).getBoolValue(),"async read disabled");
      return memory.at(name).at(read(name+".addr",4).getZExtValue()).zextOrTrunc(w);
    }
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
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for(auto [name,ram]:rams) {
      auto writer=key(ram.getResult(1));
      if(read(writer+".en",1).getBoolValue()&&read(writer+".mask",1).getBoolValue())
        memory[name].at(read(writer+".addr",4).getZExtValue())=read(writer+".data",1);
    }
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDIngressOrder(c,error)),error);require(succeeded(verify(*root)),"invalid order IR");
  Interpreter sim(named(c,"GGFASEDIngressOrder20"));require(sim.rams.size()==2&&sim.state.size()==8,"bank RAM/state missing");
  std::deque<bool> bank[2];bool ep=false,dp=false;std::mt19937_64 random(213);
  unsigned cycles=0,dual=0,partial=0,full=0,empty=0,resetWrites=0,wraps=0;
  auto cycle=[&](bool reset,bool av,bool bv,bool ready){
    ++cycles;sim.memo.clear();for(auto [n,v]:std::initializer_list<std::pair<unsigned,bool>>{{1,reset},{2,av},{3,bv},{4,ready}})sim.put(sim.arg(n),"",v);
    bool swap=ep!=!av;unsigned ai=swap?1:0,bi=swap?0:1;
    bool ar=bank[ai].size()<10,br=bank[bi].size()<10,dv=!bank[dp].empty();
    for(auto [n,v]:std::initializer_list<std::pair<unsigned,bool>>{{5,ar},{6,br},{7,dv}})require(sim.eval(sim.arg(n)).getBoolValue()==v,"bank routing/ready/valid differs");
    if(dv)require(sim.eval(sim.arg(8)).getBoolValue()==bank[dp].front(),"read/write order differs");
    bool ap=av&&ar,bp=bv&&br,pop=ready&&dv;
    dual+=ap&&bp;partial+=av&&bv&&(ap!=bp);full+=(!ar&&av)||(!br&&bv);empty+=ready&&!dv;
    auto expectedMemory=sim.memory;
    for(auto [name,ram]:sim.rams){auto w=sim.key(ram.getResult(1));if(sim.read(w+".en",1).getBoolValue()){
      auto address=sim.read(w+".addr",4).getZExtValue();require(address<10,"pointer escaped bank depth");
      expectedMemory[name][address]=sim.read(w+".data",1);resetWrites+=reset;wraps+=!reset&&address==9;}}
    if(pop)bank[dp].pop_front();if(ap)bank[ai].push_back(true);if(bp)bank[bi].push_back(false);
    if(ap!=bp)ep=!ep;if(pop)dp=!dp;if(reset){bank[0].clear();bank[1].clear();ep=dp=false;}
    sim.edge();require(sim.memory==expectedMemory,"reset cleared RAM or suppressed accepted write");
  };
  cycle(true,false,false,false);
  for(unsigned round=0;round<20;++round){for(unsigned j=0;j<12;++j)cycle(false,true,true,false);for(unsigned j=0;j<23;++j)cycle(false,false,false,true);cycle(false,true,true,true);cycle(true,true,true,true);}
  for(unsigned j=0;j<20000;++j)cycle(j%997==0,random()&1,random()&1,random()&1);
  require(dual&&partial&&full&&empty&&resetWrites&&wraps>30,"boundary/reset/wrap coverage missing");
  auto wrapper=named(c,"GGFASEDIngressOrderWrapper");Interpreter wired(wrapper);InstanceOp inner,order;
  for(auto i:wrapper.getOps<InstanceOp>())if(i.getName()=="sim")inner=i;else order=i;
  require(wrapper.getNumPorts()==7,"boundary count differs");
  for(unsigned flags=0;flags<128;++flags){wired.memo.clear();auto f=[&](unsigned i){return bool(flags>>i&1);};
    wired.put(inner.getResult(1),"",f(0));wired.put(inner.getResult(2),"",f(1));wired.put(inner.getResult(3),"writeReqDone",f(2));wired.put(wired.arg(5),"ready",f(3));
    for(unsigned j=1;j<5;++j)require(wired.output(order.getResult(j),"").getBoolValue()==f(j-1),"order input binding differs");
    for(unsigned j=5;j<9;++j)wired.put(order.getResult(j),"",f(j-4));
    require(wired.output(wired.arg(6),"read").getBoolValue()==f(1)&&wired.output(wired.arg(6),"write").getBoolValue()==f(2),"enqueue readiness differs");
    require(wired.output(wired.arg(5),"valid").getBoolValue()==f(3)&&wired.output(wired.arg(5),"bits").getBoolValue()==f(4),"order dequeue binding differs");
  }
  require(wired.drivers.at(wired.key(order.getResult(0)))==wired.arg(0),"host clock differs");
  const llvm::StringRef targets[]{"~GGFASEDIngressOrderWrapper","~GGFASEDIngressOrderWrapper|GGFASEDIngressOrderWrapper>other","~GGFASEDIngressOrderWrapper|GGFASEDIngressOrderWrapper>fased_ingress_credits.writeReqDone","~GGFASEDIngressOrderWrapper|GGFASEDTokenEngine"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==4,"lost annotations");for(unsigned j=0;j<4;++j)require(cast<DictionaryAttr>(raw[j]).getAs<StringAttr>("target").getValue()==targets[j],"target transfer differs");
  llvm::outs()<<cycles<<" bank-order cycles, "<<partial<<" partial dual enqueues, "<<wraps<<" wraps and 128 binding cases passed\n";
}
void rejection(MLIRContext &ctx){
  for(unsigned mode=1;mode<=18;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDIngressOrder(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;require(succeeded(goldengate::addFASEDIngressOrder(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDIngressOrder(c,error)),"repeat accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"19 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
