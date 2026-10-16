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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned mode=0,bool selected=false,unsigned reuse=1,unsigned flight=10) {
  llvm::StringRef rootName=selected?"GGSelectedPrintFASEDReadSchedulerWrapper":"GGFASEDReadSchedulerWrapper";
  std::string input="module { firrtl.circuit \"" + rootName.str() +
      "\" { firrtl.module @" + rootName.str() + "() {} } }";
  auto root=parseSourceString<ModuleOp>(input,&ctx);
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
    if(mode!=20)engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",attrs({{"maxWriteTransfer",mode==19?4:8},{"idReuse",mode==10?3:static_cast<int>(reuse)},{"maxFlight",mode==21?0:mode==22?-1:static_cast<int>(flight)}})),b.getNamedAttr("axi4Widths",attrs({{"addrBits",35},{"dataBits",64},{"idBits",4}}))}));}
  if(mode>=23&&mode<=26) {
    auto engine=named(c,"GGFASEDTokenEngine");
    NamedAttrList key(engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
    if(mode==23)edge.erase("idReuse");
    if(mode==24)edge.set("maxFlight",b.getStringAttr("10"));
    if(mode==25||mode==26)edge.set(mode==25?"idReuse":"maxFlight",
      b.getIntegerAttr(b.getIntegerType(128),APInt(128,mode==25?1:10)));
    key.set("axi4Edge",edge.getDictionary(&ctx));
    engine->setAttr("goldengate.bridgeConstructor",key.getDictionary(&ctx));
  }
  if(mode==12||mode==13)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==12?"GGFASEDWriteEgress":selected?"GGSelectedPrintFASEDWriteEgressWrapper":"GGFASEDWriteEgressWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==14){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}if(mode==15)c.setName("other");
  SmallVector<Attribute> raw;for(auto n:{"other","fased_write_egress_valid","fased_host_write_responses","fased_host_write_responses.bReady","fased_host_write_responses.bValid"})raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~"+rootName.str()+"|"+rootName.str()+">"+std::string(n)))}));
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
void behavior(MLIRContext &ctx,bool selected,unsigned reuse,unsigned flight) {
  auto root=fixture(ctx,0,selected,reuse,flight);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);require(succeeded(verify(*root)),"invalid write egress IR");
  Interpreter sim(named(c,"GGFASEDWriteEgress"));std::map<std::string,Value> regs;
  for(auto r:sim.module.getOps<RegResetOp>())regs[r.getName().str()]=r.getResult();for(auto r:sim.module.getOps<RegOp>())regs[r.getName().str()]=r.getResult();
  require(regs.size()==19,"write egress state differs");
  const unsigned counterWidth=reuse==1?1:2,limit=(1<<counterWidth)-1;
  for(unsigned j=0;j<16;++j)require(sim.width(regs.at("ackCounters_"+std::to_string(j)))==counterWidth,"constructor counter width differs");
  std::mt19937_64 random(218);unsigned cases=0,cancelled=0,hostRetries=0,priority=0,wraps=0,oldSample=0,evenNonzero=0,underflows=0;
  auto sample=[&](unsigned active,unsigned ack,unsigned id,unsigned enqId,unsigned newId,unsigned flags,uint64_t mask){
    sim.memo.clear();sim.state[regs.at("currReqReg_valid")]=APInt(1,active);sim.state[regs.at("currReqReg_bits")]=APInt(4,id);sim.state[regs.at("haveAck")]=APInt(1,ack);
    for(unsigned j=0;j<16;++j)sim.state[regs.at("ackCounters_"+std::to_string(j))]=APInt(counterWidth,(mask>>(j*counterWidth))&limit);
    bool reset=flags&1,fire=flags>>1&1,request=flags>>2&1,enq=flags>>3&1,ready=flags>>4&1;
    sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(3),"valid",request);sim.put(sim.arg(3),"bits",newId);
    sim.put(sim.arg(4),"valid",enq);sim.put(sim.arg(4),"bits.id",enqId);sim.put(sim.arg(4),"bits.user",random()&1);sim.put(sim.arg(4),"bits.resp",random()&3);sim.put(sim.arg(5),"tReady",ready);
    bool start=fire&&request,retire=fire&&active&&ack&&ready,retry=active&&!ack,cancel=retire&&enq&&id==enqId;
    unsigned selected=retry?id:newId;
    auto count=[&](unsigned j){return unsigned((mask>>(j*counterWidth))&limit);};
    auto expected=[&](unsigned j){
      int delta=cancel?0:enq&&enqId==j?1:retire&&id==j?-1:0;
      return (count(j)+delta)&limit;
    };
    require(sim.output(sim.arg(4),"ready",1).getBoolValue(),"host B backpressured");
    require(sim.output(sim.arg(5),"hValid",1).getBoolValue()==(!active||ack),"write token validity differs");
    require(sim.output(sim.arg(5),"tBits.id",4).getZExtValue()==id,"response ID differs");
    require(sim.output(sim.arg(5),"tBits.user",1).isZero()&&sim.output(sim.arg(5),"tBits.resp",2).isZero(),"response metadata not normalized");
    sim.edge();require(sim.state[regs.at("currReqReg_valid")].getZExtValue()==(reset?0:start?1:retire?0:active),"start/reset/retire priority differs");
    require(sim.state[regs.at("currReqReg_bits")].getZExtValue()==(start?newId:id),"unreset ID differs");
    require(sim.state[regs.at("haveAck")].getZExtValue()==(reset?0:(retry||start)?(count(selected)!=0):ack),"old counter sampling or retry ID priority differs");
    for(unsigned j=0;j<16;++j)require(sim.state[regs.at("ackCounters_"+std::to_string(j))].getZExtValue()==(reset?0:expected(j)),"counter update/cancel/wrap differs");
    ++cases;cancelled+=cancel;hostRetries+=retry&&!fire;priority+=start&&retire;wraps+=enq&&count(enqId)==limit&&!cancel;underflows+=retire&&!cancel&&count(id)==0;evenNonzero+=(retry||start)&&count(selected)==2&&!reset;oldSample+=(retry||start)&&enq&&selected==enqId;
  };
  for(unsigned active=0;active<2;++active)for(unsigned ack=0;ack<2;++ack)for(unsigned id=0;id<16;++id)for(unsigned enqId=0;enqId<16;++enqId)for(unsigned flags=0;flags<32;++flags)
    sample(active,ack,id,enqId,random()%16,flags,random());
  // Continuous state sequences also exercise one-host-cycle response arrival
  // latency, stale haveAck, reset traffic and repeated wrapping acknowledgements.
  for(unsigned j=0;j<10000;++j){uint64_t mask=0;for(unsigned k=0;k<16;++k)mask|=sim.state[regs.at("ackCounters_"+std::to_string(k))].getZExtValue()<<(k*counterWidth);
    sample(sim.state[regs.at("currReqReg_valid")].getZExtValue(),sim.state[regs.at("haveAck")].getZExtValue(),sim.state[regs.at("currReqReg_bits")].getZExtValue(),random()%16,random()%16,random()%32,mask);}
  require(cancelled&&hostRetries&&priority&&wraps&&oldSample&&underflows&&(reuse==1||evenNonzero),"write egress coverage missing");llvm::outs()<<"idReuse="<<reuse<<" maxFlight="<<flight<<": "<<cases<<" write egress transitions; "<<cancelled<<" same-ID cancellations, "<<hostRetries<<" host-only retries, "<<priority<<" chained requests, "<<wraps<<" counter wraps, "<<oldSample<<" old counter samples, "<<evenNonzero<<" even nonzero samples, "<<underflows<<" underflows passed\n";
}
void mapping(MLIRContext &ctx,bool selected) {
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);
  llvm::StringRef prefix=selected?"GGSelectedPrint":"GG";
  Interpreter w(named(c,prefix.str()+"FASEDWriteEgressWrapper"));InstanceOp inner,egress;for(auto i:w.module.getOps<InstanceOp>())if(i.getName()=="sim")inner=i;else egress=i;
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
  const std::string refs[]{"|"+prefix.str()+"FASEDWriteEgressWrapper>other",
    "|"+prefix.str()+"FASEDWriteEgressWrapper>fased_write_egress_resp.hValid",
    "|"+prefix.str()+"FASEDReadSchedulerWrapper>fased_host_write_responses",
    "|"+prefix.str()+"FASEDWriteEgressWrapper>fased_host_write_response.ready",
    "|"+prefix.str()+"FASEDWriteEgressWrapper>fased_host_write_response.valid"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==5,"lost annotations");
  for(unsigned i=0;i<5;++i)require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue()=="~"+prefix.str()+"FASEDWriteEgressWrapper"+refs[i],"annotation transfer differs");
  llvm::outs()<<"64 wrapper mapping cases and five target transfers passed\n";
}
void rejection(MLIRContext &ctx,bool selected) {
  for(unsigned mode=1;mode<=26;++mode){auto root=fixture(ctx,mode,selected);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWriteEgress(c,error))&&!error.empty(),"invalid boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDWriteEgress(c,error)),error);{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDWriteEgress(c,error)),"repeat accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"27 atomic rejection cases passed\n";
}
}
int main(int argc, char **argv) {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    if (argc == 3) {
      auto root = parseSourceFile<ModuleOp>(argv[1], &ctx);
      require(bool(root) && succeeded(verify(*root)), "production write egress boundary invalid");
      auto circuit = *root->getOps<CircuitOp>().begin();
      require(circuit.getName() == "GGFASEDWriteEgressWrapper" ||
              circuit.getName() == "GGSelectedPrintFASEDWriteEgressWrapper",
              "expected FASED write egress boundary");
      auto isolated = ModuleOp::create(circuit.getLoc()); OpBuilder b(&ctx);
      b.setInsertionPointToEnd(isolated.getBody());
      auto exported = b.create<CircuitOp>(circuit.getLoc(), b.getStringAttr("GGFASEDWriteEgress"));
      b.setInsertionPointToEnd(exported.getBodyBlock());
      // Clone the production helper verbatim for a full SFC RTL differential.
      auto helper = cast<FModuleOp>(b.insert(named(circuit,"GGFASEDWriteEgress")->clone()));
      helper.setPublic();
      require(succeeded(verify(isolated)), "exported write egress invalid");
      std::error_code ec; llvm::raw_fd_ostream out(argv[2],ec);
      require(!ec,"cannot export write egress"); isolated.print(out); out << '\n';
      out.close(); isolated.erase(); return 0;
    }
    require(argc == 1, "usage: goldengate-fased-write-egress-test [boundary.mlir helper.mlir]");
    for(bool selected:{false,true}) {
      for(unsigned reuse:{1u,2u})for(unsigned flight:{1u,10u,20u})behavior(ctx,selected,reuse,flight);
      mapping(ctx,selected);rejection(ctx,selected);
    }
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
