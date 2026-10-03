// See LICENSE for license details.
#include "goldengate/FASEDWriteEgress.h"
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDReadSchedulerWrapper\" { firrtl.module @GGFASEDReadSchedulerWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();(*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());
  auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=uint(1);
  auto flat=BundleType::get(&ctx,{{b.getStringAttr("bReady"),false,bit},{b.getStringAttr("bValid"),false,uint(mode==8?2:1)}});
  const llvm::StringRef names[]{"hostClock","fased_egress_reset","fased_tfire","fased_write_egress_valid","fased_host_write_responses","other"};
  const Type types[]{ClockType::get(&ctx),bit,bit,bit,flat,uint(8)};
  const Direction dirs[]{Direction::In,Direction::Out,Direction::Out,Direction::In,Direction::In,Direction::Out};
  SmallVector<PortInfo> ports;for(unsigned j=0;j<6;++j)ports.push_back({b.getStringAttr(mode==j+1?"missing":names[j]),types[j],dirs[j]});
  if(mode==6)ports[0].type=bit;if(mode==7)ports[1].direction=Direction::In;
  if(mode>=16&&mode<=18)ports[5].name=b.getStringAttr(mode==16?"fased_host_write_response":mode==17?"fased_write_egress_req":"fased_write_egress_resp");
  auto top=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  if(mode!=9){auto engine=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    auto attrs=[&](std::initializer_list<std::pair<llvm::StringRef,int>> es){NamedAttrList list;for(auto [n,v]:es)list.set(n,b.getI64IntegerAttr(v));return list.getDictionary(&ctx);};
    if(mode!=20)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",attrs({{"maxWriteTransfer",mode==19?4:8},{"idReuse",mode==10?2:1},{"maxFlight",10}})),b.getNamedAttr("axi4Widths",attrs({{"addrBits",35},{"dataBits",64},{"idBits",4}}))}));}
  if(mode==12||mode==13)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==12?"GGFASEDWriteEgress":"GGFASEDWriteEgressWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==14){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}if(mode==15)c.setName("other");
  SmallVector<Attribute> raw;for(auto n:{"other","fased_write_egress_valid","fased_host_write_responses","fased_host_write_responses.bReady","fased_host_write_responses.bValid"})raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~GGFASEDReadSchedulerWrapper|GGFASEDReadSchedulerWrapper>"+std::string(n)))}));
  if(mode!=11)c->setAttr("rawAnnotations",b.getArrayAttr(raw));return root;
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
  require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);require(succeeded(verify(*root)),"invalid write egress IR");
  Interpreter sim(named(c,"GGFASEDWriteEgress"));std::map<std::string,Value> regs;
  for(auto r:sim.module.getOps<RegResetOp>())regs[r.getName().str()]=r.getResult();for(auto r:sim.module.getOps<RegOp>())regs[r.getName().str()]=r.getResult();
  require(regs.size()==19,"write egress state differs");
  std::mt19937_64 random(218);unsigned cases=0,cancelled=0,hostRetries=0,priority=0,wraps=0,oldSample=0;
  auto sample=[&](unsigned active,unsigned ack,unsigned id,unsigned enqId,unsigned newId,unsigned flags,unsigned mask){
    sim.memo.clear();sim.state[regs.at("currReqReg_valid")]=APInt(1,active);sim.state[regs.at("currReqReg_bits")]=APInt(4,id);sim.state[regs.at("haveAck")]=APInt(1,ack);
    for(unsigned j=0;j<16;++j)sim.state[regs.at("ackCounters_"+std::to_string(j))]=APInt(1,mask>>j&1);
    bool reset=flags&1,fire=flags>>1&1,request=flags>>2&1,enq=flags>>3&1,ready=flags>>4&1;
    sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(3),"valid",request);sim.put(sim.arg(3),"bits",newId);
    sim.put(sim.arg(4),"valid",enq);sim.put(sim.arg(4),"bits.id",enqId);sim.put(sim.arg(4),"bits.user",random()&1);sim.put(sim.arg(4),"bits.resp",random()&3);sim.put(sim.arg(5),"tReady",ready);
    bool start=fire&&request,retire=fire&&active&&ack&&ready,retry=active&&!ack,cancel=retire&&enq&&id==enqId;
    unsigned selected=retry?id:newId,nextMask=mask;
    if(!cancel){if(enq)nextMask^=1<<enqId;if(retire)nextMask^=1<<id;}
    require(sim.output(sim.arg(4),"ready",1).getBoolValue(),"host B backpressured");
    require(sim.output(sim.arg(5),"hValid",1).getBoolValue()==(!active||ack),"write token validity differs");
    require(sim.output(sim.arg(5),"tBits.id",4).getZExtValue()==id,"response ID differs");
    require(sim.output(sim.arg(5),"tBits.user",1).isZero()&&sim.output(sim.arg(5),"tBits.resp",2).isZero(),"response metadata not normalized");
    sim.edge();require(sim.state[regs.at("currReqReg_valid")].getZExtValue()==(reset?0:start?1:retire?0:active),"start/reset/retire priority differs");
    require(sim.state[regs.at("currReqReg_bits")].getZExtValue()==(start?newId:id),"unreset ID differs");
    require(sim.state[regs.at("haveAck")].getZExtValue()==(reset?0:(retry||start)?(mask>>selected&1):ack),"old counter sampling or retry ID priority differs");
    for(unsigned j=0;j<16;++j)require(sim.state[regs.at("ackCounters_"+std::to_string(j))].getZExtValue()==(reset?0:nextMask>>j&1),"counter update/cancel/wrap differs");
    ++cases;cancelled+=cancel;hostRetries+=retry&&!fire;priority+=start&&retire;wraps+=enq&&(mask>>enqId&1)&&!cancel;oldSample+=(retry||start)&&enq&&selected==enqId;
  };
  for(unsigned active=0;active<2;++active)for(unsigned ack=0;ack<2;++ack)for(unsigned id=0;id<16;++id)for(unsigned enqId=0;enqId<16;++enqId)for(unsigned flags=0;flags<32;++flags)
    sample(active,ack,id,enqId,random()%16,flags,random()&65535);
  // Continuous state sequences also exercise one-host-cycle response arrival
  // latency, stale haveAck, reset traffic and repeated wrapping acknowledgements.
  for(unsigned j=0;j<10000;++j){unsigned mask=0;for(unsigned k=0;k<16;++k)mask|=sim.state[regs.at("ackCounters_"+std::to_string(k))].getZExtValue()<<k;
    sample(sim.state[regs.at("currReqReg_valid")].getZExtValue(),sim.state[regs.at("haveAck")].getZExtValue(),sim.state[regs.at("currReqReg_bits")].getZExtValue(),random()%16,random()%16,random()%32,mask);}
  require(cancelled&&hostRetries&&priority&&wraps&&oldSample,"write egress coverage missing");llvm::outs()<<cases<<" write egress transitions; "<<cancelled<<" same-ID cancellations, "<<hostRetries<<" host-only retries, "<<priority<<" chained requests, "<<wraps<<" counter wraps, "<<oldSample<<" old counter samples passed\n";
}
void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);
  Interpreter w(named(c,"GGFASEDWriteEgressWrapper"));InstanceOp inner,egress;for(auto i:w.module.getOps<InstanceOp>())if(i.getName()=="sim")inner=i;else egress=i;
  require(w.module.getNumPorts()==7,"wrapper port count differs");require(w.drivers.at(w.key(egress.getResult(0)))==w.arg(0),"clock mapping differs");
  for(unsigned flags=0;flags<64;++flags){w.memo.clear();auto f=[&](unsigned i){return flags>>i&1;};
    w.put(inner.getResult(1),"",f(0));w.put(inner.getResult(2),"",f(1));w.put(egress.getResult(5),"hValid",f(2));w.put(w.arg(6),"tReady",f(3));w.put(w.arg(4),"valid",f(4));w.put(egress.getResult(4),"ready",f(5));
    require(w.output(egress.getResult(1),"",1).getZExtValue()==f(0)&&w.output(egress.getResult(2),"",1).getZExtValue()==f(1),"qualified reset/fire binding differs");
    require(w.output(inner.getResult(3),"",1).getZExtValue()==f(2)&&w.output(egress.getResult(5),"tReady",1).getZExtValue()==f(3),"token readiness/response flip differs");
    require(w.output(inner.getResult(4),"bValid",1).getZExtValue()==f(4)&&w.output(inner.getResult(4),"bReady",1).getZExtValue()==f(5),"host counter acceptance mapping differs");
    require(w.output(egress.getResult(4),"valid",1).getZExtValue()==f(4)&&w.output(w.arg(4),"ready",1).getZExtValue()==f(5),"host response flip mapping differs");
    for(unsigned id=0;id<16;++id){w.put(w.arg(4),"bits.id",id);w.put(w.arg(5),"bits",id);w.put(egress.getResult(5),"tBits.id",id);
      require(w.output(egress.getResult(4),"bits.id",4).getZExtValue()==id&&w.output(egress.getResult(3),"bits",4).getZExtValue()==id&&w.output(w.arg(6),"tBits.id",4).getZExtValue()==id,"request/response payload binding differs");}
  }
  const llvm::StringRef refs[]{"|GGFASEDWriteEgressWrapper>other","|GGFASEDWriteEgressWrapper>fased_write_egress_resp.hValid","|GGFASEDReadSchedulerWrapper>fased_host_write_responses","|GGFASEDWriteEgressWrapper>fased_host_write_response.ready","|GGFASEDWriteEgressWrapper>fased_host_write_response.valid"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==5,"lost annotations");for(unsigned i=0;i<5;++i)require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue()=="~GGFASEDWriteEgressWrapper"+refs[i].str(),"annotation transfer differs");
  llvm::outs()<<"64 wrapper mapping cases and five target transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=20;++mode){auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWriteEgress(c,error))&&!error.empty(),"invalid boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWriteEgress(c,error)),"repeat accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"21 atomic rejection cases passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{behavior(ctx);mapping(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}return 0;}
