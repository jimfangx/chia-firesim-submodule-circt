// See LICENSE for license details.
#include "goldengate/FASEDIngressCredits.h"
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDIngressARQueueWrapper\" { firrtl.module @GGFASEDIngressARQueueWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse"); auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());
  auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> e;for(auto [n,w]:fields)e.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(&ctx,e);
  };
  auto address=bundle({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=bundle({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto token=[&](BundleType t){return BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,t}});};
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
      {b.getStringAttr("fased_ingress_reset"),bit,Direction::Out},
      {b.getStringAttr("fased_ingress_aw_enq_fire"),bit,Direction::Out},
      {b.getStringAttr("fased_ingress_w_last_fire"),bit,Direction::Out},
      {b.getStringAttr("fased_ingress_aw_deq"),token(address),Direction::Out},
      {b.getStringAttr("fased_ingress_w_deq"),token(data),Direction::Out},
      {b.getStringAttr("other"),uint(8),Direction::Out}};
  if(mode>=1&&mode<=6)ports[mode-1].name=b.getStringAttr("missing");
  if(mode==7)ports[1].direction=Direction::In;
  if(mode==8)ports[3].type=uint(2);
  if(mode==9)ports[4].type=token(data);
  if(mode==10)ports[5].direction=Direction::In;
  if(mode==11)ports[6].name=b.getStringAttr("fased_ingress_relaxed");
  if(mode==12)ports[6].name=b.getStringAttr("fased_ingress_credits");
  auto inner=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  if(mode!=13){
    auto engine=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDTokenEngine"),ConventionAttr::get(&ctx,Convention::Internal),SmallVector<PortInfo>{});
    if(mode!=14)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(mode==15?8:10))}))}));
  }
  if(mode==16||mode==17)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==16?"GGFASEDIngressCredits":"GGFASEDIngressCreditsWrapper"),ConventionAttr::get(&ctx,Convention::Internal),SmallVector<PortInfo>{});
  if(mode==18){b.setInsertionPointToStart(inner.getBodyBlock());b.create<InstanceOp>(c.getLoc(),inner,"used");}
  if(mode==19)c.setName("other");
  SmallVector<Attribute> raw;
  for(auto suffix:{"","|GGFASEDIngressARQueueWrapper>other","|GGFASEDIngressARQueueWrapper>fased_ingress_aw_deq.bits.addr","|GGFASEDTokenEngine"})
    raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr(std::string("~GGFASEDIngressARQueueWrapper")+suffix))}));
  if(mode!=20)c->setAttr("rawAnnotations",b.getArrayAttr(raw));return root;
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
  require(succeeded(goldengate::addFASEDIngressCredits(c,error)),error);
  require(succeeded(verify(*root)),"invalid credit IR");
  Interpreter sim(named(c,"GGFASEDIngressCredits"));SmallVector<Value> regs;
  for(auto r:sim.module.getOps<RegResetOp>())regs.push_back(r.getResult());
  require(regs.size()==2,"counter state missing");unsigned cases=0;
  for(unsigned aw=0;aw<16;++aw)for(unsigned w=0;w<16;++w)for(unsigned flags=0;flags<64;++flags){
    sim.memo.clear();sim.state[regs[0]]=APInt(4,aw);sim.state[regs[1]]=APInt(4,w);
    auto f=[&](unsigned i){return bool(flags>>i&1);};
    sim.put(sim.arg(1),"",f(0));sim.put(sim.arg(2),"",f(1));
    for(unsigned i=3;i<=6;++i)sim.put(sim.arg(i),"",f(i-1));
    bool done=(aw>w&&f(2))||(aw<w&&f(3))||(f(2)&&f(3));
    for(auto [n,v]:std::initializer_list<std::pair<llvm::StringRef,unsigned>>{
        {"awValue",aw},{"wValue",w},{"awEmpty",aw==0},{"wEmpty",w==0},{"writeReqDone",done}})
      require(sim.output(sim.arg(7),n).getZExtValue()==v,"credit status differs");
    auto next=[&](unsigned n,bool inc,bool dec){
      if(f(0))return 0u;if(inc==dec)return n;
      return inc ? (n<10?n+1:n) : (n?n-1:n);
    };
    sim.edge();
    require(sim.state[regs[0]].getZExtValue()==next(aw,f(3),f(1)?f(4):done),"AW credit differs");
    require(sim.state[regs[1]].getZExtValue()==next(w,f(2),f(1)?f(5):done),"W credit differs");++cases;
  }
  auto wrapper=named(c,"GGFASEDIngressCreditsWrapper");Interpreter wired(wrapper);
  InstanceOp inner,credit;for(auto i:wrapper.getOps<InstanceOp>()){if(i.getName()=="sim")inner=i;else credit=i;}
  require(wrapper.getPorts().size()==9,"boundary count differs");
  for(unsigned flags=0;flags<512;++flags){
    wired.memo.clear();auto f=[&](unsigned i){return bool(flags>>i&1);};
    wired.put(inner.getResult(1),"",f(0));wired.put(inner.getResult(2),"",f(1));wired.put(inner.getResult(3),"",f(2));
    wired.put(wired.arg(7),"",f(3));
    wired.put(inner.getResult(4),"valid",f(4));wired.put(wired.arg(4),"ready",f(5));
    wired.put(inner.getResult(5),"valid",f(6));wired.put(wired.arg(5),"ready",f(7));wired.put(inner.getResult(5),"bits.last",f(8));
    const unsigned expected[]{0,3,1,2};
    for(unsigned i=1;i<=4;++i)require(wired.output(credit.getResult(i),"").getBoolValue()==f(expected[i-1]),"credit reset/enqueue/relaxed wire differs");
    require(wired.output(credit.getResult(5),"").getBoolValue()==(f(4)&&f(5)),"AW accepted retirement differs");
    require(wired.output(credit.getResult(6),"").getBoolValue()==(f(6)&&f(7)&&f(8)),"final W accepted retirement differs");
    require(wired.output(inner.getResult(4),"ready").getBoolValue()==f(5),"AW ready not preserved");
    require(wired.output(inner.getResult(5),"ready").getBoolValue()==f(7),"W ready not preserved");
    for(auto n:{"awValue","wValue","awEmpty","wEmpty","writeReqDone"}){
      wired.put(credit.getResult(7),n,1);require(wired.output(wired.arg(8),n).getZExtValue()==1,"status wire differs");
    }
  }
  // Clock binding is a ground SSA connection, never an inferred target clock.
  require(wired.drivers.at(wired.key(credit.getResult(0)))==wired.arg(0),"host clock differs");
  const llvm::StringRef targets[]{"~GGFASEDIngressCreditsWrapper","~GGFASEDIngressCreditsWrapper|GGFASEDIngressCreditsWrapper>other","~GGFASEDIngressCreditsWrapper|GGFASEDIngressCreditsWrapper>fased_ingress_aw_deq.bits.addr","~GGFASEDIngressCreditsWrapper|GGFASEDTokenEngine"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==4,"lost annotations");
  for(unsigned i=0;i<4;++i)require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue()==targets[i],"target transfer differs");
  llvm::outs()<<cases<<" credit transitions and 512 wrapper cases passed\n";
}
void rejection(MLIRContext &ctx){
  for(unsigned mode=1;mode<=20;++mode){
    auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::addFASEDIngressCredits(c,error))&&!error.empty(),"malformed boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
  require(succeeded(goldengate::addFASEDIngressCredits(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::addFASEDIngressCredits(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"21 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{behavior(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;
}
