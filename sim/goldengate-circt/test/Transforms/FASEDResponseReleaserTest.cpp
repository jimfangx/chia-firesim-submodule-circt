// See LICENSE for license details.
#include "goldengate/FASEDResponseReleaser.h"
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDWriteEgressWrapper\" { firrtl.module @GGFASEDWriteEgressWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse"); auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  auto *cx = &ctx; OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin()); auto loc=c.getLoc();
  auto uint = [&](unsigned w) { return UIntType::get(cx, w, false); }; auto bit = uint(1);
  auto payload = [&](std::initializer_list<std::pair<llvm::StringRef, unsigned>> fields) {
    SmallVector<BundleType::BundleElement> es;
    for (auto [n, w] : fields) es.push_back({b.getStringAttr(n), false, uint(w)});
    return BundleType::get(cx, es);
  };
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> es) { return BundleType::get(cx, es); };
  auto decoupled = [&](BundleType bits, bool flipped = false) {
    return bundle({{b.getStringAttr("ready"), !flipped, bit}, {b.getStringAttr("valid"), flipped, bit},
                   {b.getStringAttr("bits"), flipped, bits}});
  };
  auto valid = bundle({{b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(4)}});
  auto address = payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
                          {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data = payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto write = payload({{"user",1},{"id",4},{"resp",2}});
  auto read = payload({{"user",1},{"id",4},{"last",1},{"data",64},{"resp",2}});
  auto response = [&](BundleType bits) {
    return bundle({{b.getStringAttr("tReady"), true, bit}, {b.getStringAttr("hValid"), false, bit},
                   {b.getStringAttr("tBits"), false, bits}});
  };
  auto requests = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                         {b.getStringAttr("w"), false, decoupled(data)},
                         {b.getStringAttr("ar"), false, decoupled(address)}});
  auto timing = bundle({{b.getStringAttr("aw"), false, decoupled(address)},
                       {b.getStringAttr("w"), false, decoupled(data)},
                       {b.getStringAttr("b"), false, decoupled(write, true)},
                       {b.getStringAttr("ar"), false, decoupled(address)},
                       {b.getStringAttr("r"), false, decoupled(read, true)}});
  const llvm::StringRef names[]{"hostClock", "fased_model_reset", "fased_tfire", "fased_timing",
      "fased_read_egress_req", "fased_read_egress_resp", "fased_write_egress_req", "fased_write_egress_resp"};
  const Type types[]{ClockType::get(cx), bit, bit, timing, valid, response(read), valid, response(write)};
  const Direction dirs[]{Direction::In, Direction::Out, Direction::Out, Direction::Out,
                        Direction::In, Direction::Out, Direction::In, Direction::Out};

  SmallVector<PortInfo> ps;
  for(unsigned i=0;i<8;++i) ps.push_back({b.getStringAttr(mode==i+1 ? "missing" : names[i]),types[i],dirs[i]});
  ps.push_back({b.getStringAttr("other"),uint(8),Direction::Out});
  if(mode==9) ps[3].type=bit; if(mode==10) ps[5].direction=Direction::In;
  if(mode>=19 && mode<=21) ps[8].name=b.getStringAttr(mode==19 ? "fased_timing_requests" : mode==20 ? "fased_next_read" : "fased_next_write");
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(cx,Convention::Internal),ps);
  if(mode!=11) {
    auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    if(mode!=18) engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({
      b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(mode==17?32:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));
  }
  if(mode==13 || mode==14) b.create<FModuleOp>(loc,b.getStringAttr(mode==13?"GGFASEDResponseReleaser":"GGFASEDResponseReleaserWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==15){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==16)c.setName("wrong");
  SmallVector<Attribute> raw;
  for(auto n:{"other","fased_timing.aw.bits.id","fased_timing.w.ready","fased_timing.ar.valid","fased_timing.r.bits.data","fased_read_egress_req.valid","fased_write_egress_resp.tBits.id"})
    raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~GGFASEDWriteEgressWrapper|GGFASEDWriteEgressWrapper>"+std::string(n)))}));
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr(raw)); return root;
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
  require(succeeded(goldengate::addFASEDResponseReleaser(c,error)),error); require(succeeded(verify(*root)),"invalid releaser IR");
  Interpreter sim(named(c,"GGFASEDResponseReleaser")); std::map<std::string,Value> regs;
  for(auto r:sim.module.getOps<RegResetOp>()) regs[r.getName().str()]=r.getResult();
  require(regs.size()==2,"releaser has extra metadata state");
  auto rf=regs.at("currentRead_full"),wf=regs.at("currentWrite_full");
  std::mt19937_64 random(219); unsigned cases=0,stalls=0,refills=0,nonLast=0,blocked=0,stalledReset=0;
  auto sample=[&](bool readFull,bool writeFull,unsigned id,unsigned flags) {
    sim.memo.clear();sim.state[rf]=APInt(1,readFull);sim.state[wf]=APInt(1,writeFull);
    bool reset=flags&1,fire=flags>>1&1,nr=flags>>2&1,nw=flags>>3&1,rr=flags>>4&1,br=flags>>5&1,last=flags>>6&1;
    sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(3),"ready",br);sim.put(sim.arg(4),"ready",rr);
    sim.put(sim.arg(7),"valid",nr);sim.put(sim.arg(7),"bits.id",id);sim.put(sim.arg(7),"bits.len",random()&255);
    sim.put(sim.arg(8),"valid",nw);sim.put(sim.arg(8),"bits.id",15-id);
    uint64_t data=random();unsigned rid=random()%16,bid=random()%16,rresp=random()%4,bresp=random()%4,ru=random()%2,bu=random()%2;
    sim.put(sim.arg(6),"rBits.data",data);sim.put(sim.arg(6),"rBits.last",last);sim.put(sim.arg(6),"rBits.id",rid);sim.put(sim.arg(6),"rBits.resp",rresp);sim.put(sim.arg(6),"rBits.user",ru);
    sim.put(sim.arg(6),"bBits.id",bid);sim.put(sim.arg(6),"bBits.resp",bresp);sim.put(sim.arg(6),"bBits.user",bu);
    bool rpop=readFull&&rr&&last,bpop=writeFull&&br,rready=!readFull||rpop,bready=!writeFull||bpop,rpush=rready&&nr,bpush=bready&&nw;
    require(sim.output(sim.arg(7),"ready",1).getBoolValue()==rready&&sim.output(sim.arg(8),"ready",1).getBoolValue()==bready,"pipe queue capacity differs");
    require(sim.output(sim.arg(5),"r.valid",1).getBoolValue()==rpush&&sim.output(sim.arg(5),"b.valid",1).getBoolValue()==bpush,"requests not accepted exactly once");
    require(sim.output(sim.arg(5),"r.bits",4).getZExtValue()==id&&sim.output(sim.arg(5),"b.bits",4).getZExtValue()==15-id,"request ID changed");
    require(sim.output(sim.arg(4),"valid",1).getBoolValue()==readFull&&sim.output(sim.arg(3),"valid",1).getBoolValue()==writeFull,"response bypassed empty queue");
    require(sim.output(sim.arg(6),"rReady",1).getBoolValue()==rr&&sim.output(sim.arg(6),"bReady",1).getBoolValue()==br,"target ready feedback differs");
    require(sim.output(sim.arg(4),"bits.data",64).getZExtValue()==data&&sim.output(sim.arg(4),"bits.last",1).getBoolValue()==last&&sim.output(sim.arg(4),"bits.id",4).getZExtValue()==rid&&sim.output(sim.arg(4),"bits.resp",2).getZExtValue()==rresp&&sim.output(sim.arg(4),"bits.user",1).getZExtValue()==ru,"read payload differs");
    require(sim.output(sim.arg(3),"bits.id",4).getZExtValue()==bid&&sim.output(sim.arg(3),"bits.resp",2).getZExtValue()==bresp&&sim.output(sim.arg(3),"bits.user",1).getZExtValue()==bu,"write payload differs");
    sim.edge();
    require(sim.state[rf].getBoolValue()==(fire?(reset?false:rpush?true:rpop?false:readFull):readFull),"read target edge/reset/refill differs");
    require(sim.state[wf].getBoolValue()==(fire?(reset?false:bpush?true:bpop?false:writeFull):writeFull),"write target edge/reset/refill differs");
    ++cases;stalls+=!fire;refills+=fire&&rpop&&rpush;nonLast+=readFull&&rr&&!last;blocked+=nr&&!rready;stalledReset+=reset&&!fire;
  };
  for(unsigned rf=0;rf<2;++rf)for(unsigned wf=0;wf<2;++wf)for(unsigned id=0;id<16;++id)for(unsigned flags=0;flags<128;++flags)sample(rf,wf,id,flags);
  for(unsigned i=0;i<10000;++i)sample(sim.state[rf].getBoolValue(),sim.state[wf].getBoolValue(),random()%16,random()%128);
  require(stalls&&refills&&nonLast&&blocked&&stalledReset,"releaser coverage missing");
  llvm::outs()<<cases<<" target response transitions; stalls, refills, non-last beats, backpressure and stalled reset passed\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDResponseReleaser(c,error)),error);
  Interpreter w(named(c,"GGFASEDResponseReleaserWrapper"));InstanceOp sim,rel;for(auto i:w.module.getOps<InstanceOp>())if(i.getName()=="sim")sim=i;else rel=i;
  require(w.module.getNumPorts()==7,"wrapper port count differs");require(w.drivers.at(w.key(rel.getResult(0)))==w.arg(0),"clock binding differs");
  for(unsigned flags=0;flags<64;++flags){w.memo.clear();auto f=[&](unsigned i){return flags>>i&1;};
    w.put(sim.getResult(1),"",f(0));w.put(sim.getResult(2),"",f(1));
    require(w.output(rel.getResult(1),"",1).getZExtValue()==f(0)&&w.output(rel.getResult(2),"",1).getZExtValue()==f(1),"model reset or fire binding differs");
    for(bool read:{false,true}){auto ch=read?"r":"b";unsigned j=read?4:3,rq=read?4:6,rp=read?5:7,next=read?7:8;
      w.put(sim.getResult(3),std::string(ch)+".ready",f(2));w.put(rel.getResult(j),"valid",f(3));w.put(rel.getResult(5),std::string(ch)+".valid",f(4));
      w.put(rel.getResult(6),read?"rReady":"bReady",f(5));w.put(w.arg(read?5:6),"valid",f(4));w.put(rel.getResult(next),"ready",f(5));
      require(w.output(rel.getResult(j),"ready",1).getZExtValue()==f(2)&&w.output(sim.getResult(3),std::string(ch)+".valid",1).getZExtValue()==f(3),"target response handshake binding differs");
      require(w.output(sim.getResult(rq),"valid",1).getZExtValue()==f(4)&&w.output(sim.getResult(rp),"tReady",1).getZExtValue()==f(5),"egress request/ready binding differs");
      require(w.output(rel.getResult(next),"valid",1).getZExtValue()==f(4)&&w.output(w.arg(read?5:6),"ready",1).getZExtValue()==f(5),"completion metadata flip binding differs");
      for(unsigned id=0;id<16;++id){w.put(rel.getResult(5),std::string(ch)+".bits",id);w.put(sim.getResult(rp),"tBits.id",id);w.put(rel.getResult(j),"bits.id",id);
        require(w.output(sim.getResult(rq),"bits",4).getZExtValue()==id&&w.output(rel.getResult(6),read?"rBits.id":"bBits.id",4).getZExtValue()==id&&w.output(sim.getResult(3),std::string(ch)+".bits.id",4).getZExtValue()==id,"response/request payload binding differs");}
    }
    for(auto ch:{"aw","w","ar"}){w.put(sim.getResult(3),std::string(ch)+".valid",f(2));w.put(w.arg(4),std::string(ch)+".ready",f(3));
      require(w.output(w.arg(4),std::string(ch)+".valid",1).getZExtValue()==f(2)&&w.output(sim.getResult(3),std::string(ch)+".ready",1).getZExtValue()==f(3),"model request flip binding differs");}
  }
  const llvm::StringRef refs[]{"|GGFASEDResponseReleaserWrapper>other","|GGFASEDResponseReleaserWrapper>fased_timing_requests.aw.bits.id","|GGFASEDResponseReleaserWrapper>fased_timing_requests.w.ready","|GGFASEDResponseReleaserWrapper>fased_timing_requests.ar.valid","|GGFASEDWriteEgressWrapper>fased_timing.r.bits.data","|GGFASEDWriteEgressWrapper>fased_read_egress_req.valid","|GGFASEDWriteEgressWrapper>fased_write_egress_resp.tBits.id"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==7,"lost annotations");
  for(unsigned i=0;i<7;++i)require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue()=="~GGFASEDResponseReleaserWrapper"+refs[i].str(),"annotation transfer differs");
  llvm::outs()<<"64 wrapper mapping cases and seven target transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=21;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDResponseReleaser(c,error))&&!error.empty(),"invalid boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDResponseReleaser(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDResponseReleaser(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"22 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
