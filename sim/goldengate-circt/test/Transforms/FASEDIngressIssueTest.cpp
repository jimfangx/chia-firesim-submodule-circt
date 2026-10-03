// See LICENSE for license details.
#include "goldengate/FASEDIngressIssue.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDIngressOrderWrapper\" { firrtl.module @GGFASEDIngressOrderWrapper() {} } }",&ctx);require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();(*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());
  auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){SmallVector<BundleType::BundleElement> e;for(auto [n,w]:fields)e.push_back({b.getStringAttr(n),false,uint(w)});return BundleType::get(&ctx,e);};
  auto address=bundle({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=bundle({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto token=[&](FIRRTLBaseType t){return BundleType::get(&ctx,{{b.getStringAttr("ready"),mode!=15,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,t}});};
  auto status=bundle({{"awValue",4},{"wValue",4},{"awEmpty",1},{"wEmpty",1},{"writeReqDone",1}});
  auto transactions=bundle({{"arReady",1},{"arValid",1},{"rReady",1},{"rValid",1},{"rLast",1},{"awReady",1},{"awValid",1},{"bReady",1},{"bValid",1}});
  const llvm::StringRef names[]{"hostClock","fased_ingress_reset","fased_ingress_relaxed","fased_host_mem_idle","fased_host_read_inflight","fased_ingress_credits","fased_ingress_order","fased_ingress_aw_deq","fased_ingress_w_deq","fased_ingress_ar_deq","fased_host_transactions"};
  const Type types[]{ClockType::get(&ctx),bit,bit,bit,bit,status,token(bit),token(address),token(data),token(address),transactions};SmallVector<PortInfo> ports;
  for(unsigned j=0;j<11;++j)ports.push_back({b.getStringAttr(mode==j+1?"missing":names[j]),types[j],(j==0||j==2||j==10)?Direction::In:Direction::Out});
  ports.push_back({b.getStringAttr("other"),uint(8),Direction::Out});
  if(mode==12)ports[0].type=bit;if(mode==13)ports[1].direction=Direction::In;if(mode==14)ports[7].type=token(data);
  if(mode==16||mode==17)ports[11].name=b.getStringAttr(mode==16?"fased_host_requests":"fased_host_responses");
  auto top=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  if(mode!=18){auto engine=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});if(mode!=19)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxFlight",b.getI64IntegerAttr(mode==20?8:10))}))}));}
  if(mode==21||mode==22)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==21?"GGFASEDIngressIssue":"GGFASEDIngressIssueWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==23){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}if(mode==24)c.setName("other");
  SmallVector<Attribute> raw;
  for(auto ref:{"other","fased_ingress_aw_deq.bits.addr","fased_host_transactions","fased_host_transactions.arReady","fased_host_transactions.arValid","fased_host_transactions.awReady","fased_host_transactions.awValid","fased_host_transactions.rReady","fased_host_transactions.rValid","fased_host_transactions.rLast","fased_host_transactions.bReady","fased_host_transactions.bValid"})raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~GGFASEDIngressOrderWrapper|GGFASEDIngressOrderWrapper>"+std::string(ref)))}));
  if(mode!=25)c->setAttr("rawAnnotations",b.getArrayAttr(raw));return root;
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
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDIngressIssue(c,error)),error);require(succeeded(verify(*root)),"invalid issue IR");
  Interpreter sim(named(c,"GGFASEDIngressIssue"));auto reg=*sim.module.getOps<RegResetOp>().begin();require(sim.state.size()==1,"write data state differs");
  std::mt19937_64 random(214);unsigned cases=0,priority=0,stalls=0;
  for(unsigned state=0;state<2;++state)for(unsigned flags=0;flags<32768;++flags){
    sim.memo.clear();sim.state[reg.getResult()]=APInt(1,state);auto f=[&](unsigned i){return bool(flags>>i&1);};
    for(unsigned j=1;j<=6;++j)sim.put(sim.arg(j),"",f(j-1));
    sim.put(sim.arg(7),"valid",f(6));sim.put(sim.arg(7),"bits",f(7));
    sim.put(sim.arg(8),"valid",f(8));sim.put(sim.arg(9),"valid",f(10));sim.put(sim.arg(10),"valid",f(13));
    sim.put(sim.arg(11),"aw.ready",f(9));sim.put(sim.arg(11),"w.ready",f(11));sim.put(sim.arg(11),"ar.ready",f(14));sim.put(sim.arg(9),"bits.last",f(12));
    bool enables[]{f(1)?!f(4):f(2)&&f(6)&&!f(7),f(1)?!f(5):bool(state),f(1)||((f(2)||f(3))&&f(6)&&f(7))};
    const unsigned validFlags[]{8,10,13},readyFlags[]{9,11,14};bool fire[3];
    for(unsigned j=0;j<3;++j){llvm::StringRef n=j==0?"aw":j==1?"w":"ar";unsigned q=8+j;
      require(sim.output(sim.arg(11),n.str()+".valid").getBoolValue()==(enables[j]&&f(validFlags[j])),"host valid predicate differs");
      require(sim.output(sim.arg(q),"ready").getBoolValue()==(enables[j]&&f(readyFlags[j])),"queue ready predicate differs");fire[j]=enables[j]&&f(validFlags[j])&&f(readyFlags[j]);
      auto payload=cast<BundleType>(cast<BundleType>(sim.arg(q).getType()).getElements()[2].type);
      for(auto e:payload.getElements()){auto value=random();if(e.name=="last")value=f(12);auto w=*cast<UIntType>(e.type).getWidth();
        sim.put(sim.arg(q),"bits."+e.name.str(),value);require(sim.output(sim.arg(11),n.str()+".bits."+e.name.str(),w).getZExtValue()==APInt(w,value).getZExtValue(),"payload field changed");}
    }
    require(sim.output(sim.arg(7),"ready").getBoolValue()==(fire[0]||fire[2]),"order retirement differs");
    unsigned next=f(0)?0:fire[0]?1:fire[1]&&f(12)?0:state;sim.edge();require(sim.state[reg.getResult()].getZExtValue()==next,"AW/final-W priority or reset differs");
    priority+=fire[0]&&fire[1]&&f(12);stalls+=!fire[0]&&!fire[1]&&!fire[2];++cases;
  }
  require(priority&&stalls,"priority/stall coverage missing");llvm::outs()<<cases<<" issue transitions/payload cases, "<<priority<<" AW/final-W collisions passed\n";
}
void mapping(MLIRContext &ctx){
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDIngressIssue(c,error)),error);
  Interpreter wired(named(c,"GGFASEDIngressIssueWrapper"));InstanceOp inner,issue;for(auto i:wired.module.getOps<InstanceOp>())if(i.getName()=="sim")inner=i;else issue=i;
  require(wired.module.getNumPorts()==9,"wrapper port contract differs");
  require(wired.drivers.at(wired.key(issue.getResult(0)))==wired.arg(0),"host clock differs");
  for(unsigned flags=0;flags<256;++flags){wired.memo.clear();auto f=[&](unsigned i){return bool(flags>>i&1);};
    wired.put(inner.getResult(1),"",f(0));wired.put(wired.arg(2),"",f(1));wired.put(inner.getResult(3),"",f(2));wired.put(inner.getResult(4),"",f(3));wired.put(inner.getResult(5),"awEmpty",f(4));wired.put(inner.getResult(5),"wEmpty",f(5));
    for(unsigned j=1;j<=6;++j)require(wired.output(issue.getResult(j),"").getBoolValue()==f(j-1),"policy/reset input differs");
    for(auto [q,j]:{std::pair<unsigned,unsigned>{6,7},{7,8},{8,9},{9,10}}){wired.put(inner.getResult(q),"valid",f(6));wired.put(issue.getResult(j),"ready",f(7));require(wired.output(issue.getResult(j),"valid").getBoolValue()==f(6)&&wired.output(inner.getResult(q),"ready").getBoolValue()==f(7),"queue/order binding differs");}
    for(auto [n,ch,leaf]:{std::tuple<llvm::StringRef,llvm::StringRef,llvm::StringRef>{"arReady","ar","ready"},{"arValid","ar","valid"},{"awReady","aw","ready"},{"awValid","aw","valid"}}){
      if(leaf=="ready")wired.put(wired.arg(7),ch.str()+".ready",f(7));else wired.put(issue.getResult(11),ch.str()+".valid",f(6));
      require(wired.output(inner.getResult(10),n).getBoolValue()==(leaf=="ready"?f(7):f(6)),"host counter acceptance binding differs");}
    for(auto n:{"rReady","rValid","rLast","bReady","bValid"}){wired.put(wired.arg(8),n,f(7));require(wired.output(inner.getResult(10),n).getBoolValue()==f(7),"response handshake binding differs");}
  }
  const llvm::StringRef refs[]{"|GGFASEDIngressIssueWrapper>other","|GGFASEDIngressOrderWrapper>fased_ingress_aw_deq.bits.addr","|GGFASEDIngressOrderWrapper>fased_host_transactions","|GGFASEDIngressIssueWrapper>fased_host_requests.ar.ready","|GGFASEDIngressIssueWrapper>fased_host_requests.ar.valid","|GGFASEDIngressIssueWrapper>fased_host_requests.aw.ready","|GGFASEDIngressIssueWrapper>fased_host_requests.aw.valid","|GGFASEDIngressIssueWrapper>fased_host_responses.rReady","|GGFASEDIngressIssueWrapper>fased_host_responses.rValid","|GGFASEDIngressIssueWrapper>fased_host_responses.rLast","|GGFASEDIngressIssueWrapper>fased_host_responses.bReady","|GGFASEDIngressIssueWrapper>fased_host_responses.bValid"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==12,"lost annotations");for(unsigned j=0;j<12;++j)require(cast<DictionaryAttr>(raw[j]).getAs<StringAttr>("target").getValue()=="~GGFASEDIngressIssueWrapper"+refs[j].str(),"target transfer differs");
  llvm::outs()<<"256 wrapper binding cases passed\n";
}
void rejection(MLIRContext &ctx){
  for(unsigned mode=1;mode<=25;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDIngressIssue(c,error))&&!error.empty(),"invalid boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;require(succeeded(goldengate::addFASEDIngressIssue(c,error)),error);{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDIngressIssue(c,error)),"repeat accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"26 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
