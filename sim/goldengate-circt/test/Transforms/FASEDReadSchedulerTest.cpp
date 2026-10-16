// See LICENSE for license details.
#include "goldengate/FASEDReadScheduler.h"
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
  llvm::StringRef rootName = selected ? "GGSelectedPrintFASEDReadBufferWrapper"
                                      : "GGFASEDReadBufferWrapper";
  std::string input = "module { firrtl.circuit \"" + rootName.str() +
      "\" { firrtl.module @" + rootName.str() + "() {} } }";
  auto root=parseSourceString<ModuleOp>(input,&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();(*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());
  auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=uint(1);
  auto bundle=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> es){SmallVector<BundleType::BundleElement> fields;for(auto [n,w]:es)fields.push_back({b.getStringAttr(n),false,uint(w)});return BundleType::get(&ctx,fields);};
  auto stored=bundle({{"data",64},{"last",1}}),readiness=bundle({{"readValid",1},{"writeValid",1}});
  auto token=BundleType::get(&ctx,{{b.getStringAttr("ready"),mode!=9,bit},{b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,stored}});
  const llvm::StringRef names[]{"hostClock","fased_egress_reset","fased_tfire","fased_egress_readiness","fased_read_buffer_address","fased_read_buffer_deq","other"};
  const Type types[]{ClockType::get(&ctx),bit,bit,readiness,uint(4),token,uint(8)};
  const Direction dirs[]{Direction::In,Direction::Out,Direction::Out,Direction::In,Direction::In,Direction::Out,Direction::Out};
  SmallVector<PortInfo> ports;for(unsigned j=0;j<7;++j)ports.push_back({b.getStringAttr(mode==j+1?"missing":names[j]),types[j],dirs[j]});
  if(mode==7)ports[0].type=bit;if(mode==8)ports[1].direction=Direction::In;
  if(mode>=17&&mode<=19)ports[6].name=b.getStringAttr(mode==17?"fased_read_egress_req":mode==18?"fased_read_egress_resp":"fased_write_egress_valid");
  auto top=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ports);
  if(mode!=10){auto engine=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDTokenEngine"),top.getConventionAttr(),ArrayRef<PortInfo>{});
    auto attrs=[&](std::initializer_list<std::pair<llvm::StringRef,int>> es){NamedAttrList list;for(auto [n,v]:es)list.set(n,b.getI64IntegerAttr(v));return list.getDictionary(&ctx);};
    engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Edge",attrs({{"maxReadTransfer",8},{"idReuse",mode==11?3:static_cast<int>(reuse)},{"maxFlight",mode==20?0:mode==21?-1:static_cast<int>(flight)}})),b.getNamedAttr("axi4Widths",attrs({{"addrBits",35},{"dataBits",64},{"idBits",4}}))}));}
  if(mode>=22&&mode<=25) {
    auto engine=named(c,"GGFASEDTokenEngine");
    NamedAttrList key(engine->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
    NamedAttrList edge(cast<DictionaryAttr>(key.get("axi4Edge")));
    if(mode==22)edge.erase("idReuse");
    if(mode==23)edge.set("maxFlight",b.getStringAttr("10"));
    if(mode==24||mode==25)edge.set(mode==24?"idReuse":"maxFlight",
      b.getIntegerAttr(b.getIntegerType(128),APInt(128,mode==24?1:10)));
    key.set("axi4Edge",edge.getDictionary(&ctx));
    engine->setAttr("goldengate.bridgeConstructor",key.getDictionary(&ctx));
  }
  if(mode==13||mode==14)b.create<FModuleOp>(c.getLoc(),b.getStringAttr(mode==13?"GGFASEDReadScheduler":selected?"GGSelectedPrintFASEDReadSchedulerWrapper":"GGFASEDReadSchedulerWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==15){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}if(mode==16)c.setName("other");
  SmallVector<Attribute> raw;for(auto n:{"other","fased_egress_readiness","fased_egress_readiness.readValid","fased_egress_readiness.writeValid","fased_read_buffer_address","fased_read_buffer_deq.valid","fased_read_buffer_deq.bits.data","fased_read_buffer_deq.bits.last"})raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~"+c.getName().str()+"|"+c.getName().str()+">"+std::string(n)))}));
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr(raw));return root;
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
  require(succeeded(goldengate::addFASEDReadScheduler(c,error)),error);require(succeeded(verify(*root)),"invalid scheduler IR");
  Interpreter sim(named(c,"GGFASEDReadScheduler"));auto valid=*sim.module.getOps<RegResetOp>().begin();auto id=*sim.module.getOps<RegOp>().begin();require(sim.state.size()==2,"scheduler state differs");
  std::mt19937_64 random(217);unsigned cases=0,chained=0,resetStarts=0,invalidRetire=0;
  for(unsigned flags=0;flags<64;++flags)for(unsigned oldId=0;oldId<16;++oldId)for(unsigned newId=0;newId<16;++newId){
    sim.memo.clear();auto f=[&](unsigned i){return bool(flags>>i&1);};bool reset=f(0),fire=f(1),request=f(2),ready=f(3),bufferValid=f(4),last=f(5);
    // Exercise both active and inactive requests, with every ID pairing.
    for(unsigned active=0;active<2;++active){sim.memo.clear();sim.state[valid.getResult()]=APInt(1,active);sim.state[id.getResult()]=APInt(4,oldId);
      sim.put(sim.arg(1),"",reset);sim.put(sim.arg(2),"",fire);sim.put(sim.arg(3),"valid",request);sim.put(sim.arg(3),"bits",newId);
      sim.put(sim.arg(4),"valid",bufferValid);sim.put(sim.arg(4),"bits.last",last);uint64_t data=random();sim.put(sim.arg(4),"bits.data",data);sim.put(sim.arg(6),"tReady",ready);
      bool start=fire&&request,pop=fire&&active&&ready,done=pop&&last;
      require(sim.output(sim.arg(5),"",4).getZExtValue()==(start?newId:oldId),"prefetch address differs");
      require(sim.output(sim.arg(4),"ready",1).getBoolValue()==pop,"dequeue gate differs");
      require(sim.output(sim.arg(6),"hValid",1).getBoolValue()==(!active||bufferValid),"read token validity differs");
      require(sim.output(sim.arg(6),"tBits.id",4).getZExtValue()==oldId,"response ID differs");
      require(sim.output(sim.arg(6),"tBits.data").getZExtValue()==data&&sim.output(sim.arg(6),"tBits.last",1).getBoolValue()==last,"response payload differs");
      require(sim.output(sim.arg(6),"tBits.user",1).isZero()&&sim.output(sim.arg(6),"tBits.resp",2).isZero(),"response constants differ");
      sim.edge();require(sim.state[valid.getResult()].getZExtValue()==(reset?0:start?1:done?0:active),"start/retire/reset priority differs");
      require(sim.state[id.getResult()].getZExtValue()==(start?newId:oldId),"unreset request ID differs");
      ++cases;chained+=start&&done;resetStarts+=reset&&start;invalidRetire+=done&&!bufferValid;
    }
  }
  require(chained&&resetStarts&&invalidRetire,"scheduler edge coverage missing");llvm::outs()<<cases<<" scheduler edge cases, "<<chained<<" chained requests, "<<resetStarts<<" reset starts, "<<invalidRetire<<" invalid-buffer retirements passed\n";
}
void mapping(MLIRContext &ctx,bool selected) {
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();std::string error;require(succeeded(goldengate::addFASEDReadScheduler(c,error)),error);
  Interpreter w(named(c,selected?"GGSelectedPrintFASEDReadSchedulerWrapper":"GGFASEDReadSchedulerWrapper"));InstanceOp inner,scheduler;for(auto i:w.module.getOps<InstanceOp>())if(i.getName()=="sim")inner=i;else scheduler=i;
  require(w.module.getNumPorts()==7,"wrapper port count differs");require(w.drivers.at(w.key(scheduler.getResult(0)))==w.arg(0),"clock mapping differs");
  for(unsigned flags=0;flags<32;++flags){w.memo.clear();auto f=[&](unsigned i){return flags>>i&1;};
    w.put(inner.getResult(1),"",f(0));w.put(inner.getResult(2),"",f(1));w.put(w.arg(4),"",f(2));w.put(scheduler.getResult(6),"hValid",f(3));w.put(w.arg(6),"tReady",f(4));
    require(w.output(scheduler.getResult(1),"",1).getZExtValue()==f(0)&&w.output(scheduler.getResult(2),"",1).getZExtValue()==f(1),"qualified reset/fire differs");
    require(w.output(inner.getResult(3),"writeValid",1).getZExtValue()==f(2)&&w.output(inner.getResult(3),"readValid",1).getZExtValue()==f(3),"egress token readiness mapping differs");
    require(w.output(scheduler.getResult(6),"tReady",1).getZExtValue()==f(4),"response ready flip differs");
    w.put(scheduler.getResult(4),"ready",f(0));w.put(inner.getResult(5),"valid",f(1));require(w.output(inner.getResult(5),"ready",1).getZExtValue()==f(0)&&w.output(scheduler.getResult(4),"valid",1).getZExtValue()==f(1),"buffer flip mapping differs");
    w.put(scheduler.getResult(5),"",flags&15);require(w.output(inner.getResult(4),"",4).getZExtValue()==(flags&15),"address binding differs");
  }
  const llvm::StringRef refs[]{"|GGFASEDReadSchedulerWrapper>other","|GGFASEDReadBufferWrapper>fased_egress_readiness","|GGFASEDReadSchedulerWrapper>fased_read_egress_resp.hValid","|GGFASEDReadSchedulerWrapper>fased_write_egress_valid","|GGFASEDReadBufferWrapper>fased_read_buffer_address","|GGFASEDReadBufferWrapper>fased_read_buffer_deq.valid","|GGFASEDReadSchedulerWrapper>fased_read_egress_resp.tBits.data","|GGFASEDReadSchedulerWrapper>fased_read_egress_resp.tBits.last"};
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw.size()==8,"lost annotations");
  for(unsigned i=0;i<8;++i) {
    std::string suffix=refs[i].str(); if(selected) suffix.insert(3,"SelectedPrint");
    require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue()=="~"+c.getName().str()+suffix,"annotation target transfer differs");
  }
  llvm::outs()<<"32 wrapper mapping cases and eight target transfers passed\n";
}
void rejection(MLIRContext &ctx,bool selected) {
  for(unsigned mode=1;mode<=25;++mode){auto root=fixture(ctx,mode,selected);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDReadScheduler(c,error))&&!error.empty(),"invalid boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");}
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;require(succeeded(goldengate::addFASEDReadScheduler(c,error)),error);{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addFASEDReadScheduler(c,error)),"repeat accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");llvm::outs()<<"26 atomic rejection cases passed\n";
}
}
int main(int argc, char **argv) {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    if (argc == 3) {
      auto root = parseSourceFile<ModuleOp>(argv[1], &ctx);
      require(bool(root) && succeeded(verify(*root)), "production read scheduler boundary invalid");
      auto circuit = *root->getOps<CircuitOp>().begin();
      require(circuit.getName() == "GGFASEDReadSchedulerWrapper" ||
              circuit.getName() == "GGSelectedPrintFASEDReadSchedulerWrapper",
              "expected FASED read scheduler boundary");
      auto isolated = ModuleOp::create(circuit.getLoc()); OpBuilder b(&ctx);
      b.setInsertionPointToEnd(isolated.getBody());
      auto exported = b.create<CircuitOp>(circuit.getLoc(), b.getStringAttr("GGNICReadEgress"));
      b.setInsertionPointToEnd(exported.getBodyBlock());
      // Export the actual emitted scheduler and RAM/pointer operations. This
      // test top only composes them using the production wrapper connections.
      auto scheduler = cast<FModuleOp>(b.insert(named(circuit,"GGFASEDReadScheduler")->clone()));
      FModuleOp productionBuffer;
      for(auto m:circuit.getOps<FModuleOp>())
        if(m.getName()=="GGFASEDReadBuffer16x8"||m.getName()=="GGFASEDReadBuffer16x16") {
          require(!productionBuffer,"ambiguous read buffer");productionBuffer=m;
        }
      require(bool(productionBuffer),"missing production read buffer");
      auto buffer = cast<FModuleOp>(b.insert(productionBuffer->clone()));
      SmallVector<PortInfo> ports;
      auto port = [&](llvm::StringRef name, FModuleOp m, unsigned index) {
        auto p = m.getPorts()[index]; p.name = b.getStringAttr(name); ports.push_back(p);
      };
      port("clock",scheduler,0); port("reset",scheduler,1); port("targetFire",scheduler,2);
      port("req",scheduler,3); port("resp",scheduler,6);
      port("enq",buffer,2); port("enqAddr",buffer,4);
      auto top = b.create<FModuleOp>(circuit.getLoc(),b.getStringAttr("GGNICReadEgress"),
                                    scheduler.getConventionAttr(),ports);
      top.setPublic(); b.setInsertionPointToEnd(top.getBodyBlock());
      auto s = b.create<InstanceOp>(circuit.getLoc(),scheduler,"readScheduler");
      auto q = b.create<InstanceOp>(circuit.getLoc(),buffer,"readBuffer");
      auto arg = [&](unsigned i) {return top.getBodyBlock()->getArgument(i);};
      auto connect = [&](Value d, Value v) {b.create<ConnectOp>(circuit.getLoc(),d,v);};
      for (unsigned i=0;i<4;++i) connect(s.getResult(i),arg(i));
      connect(arg(4),s.getResult(6)); connect(s.getResult(4),q.getResult(3));
      connect(q.getResult(5),s.getResult(5));
      connect(q.getResult(0),arg(0)); connect(q.getResult(1),arg(1));
      connect(q.getResult(4),arg(6));
      auto field = [&](Value v, llvm::StringRef name) {
        return b.create<SubfieldOp>(circuit.getLoc(),v,name).getResult();
      };
      for (auto name : {"valid","bits"}) connect(field(q.getResult(2),name),field(arg(5),name));
      auto one = b.create<ConstantOp>(circuit.getLoc(),UIntType::get(&ctx,1,false),APInt(1,1));
      connect(field(arg(5),"ready"),one);
      require(succeeded(verify(isolated)), "exported read egress invalid");
      std::error_code ec; llvm::raw_fd_ostream out(argv[2],ec);
      require(!ec,"cannot export read egress"); isolated.print(out); out << '\n';
      out.close(); isolated.erase(); return 0;
    }
    require(argc==1,"usage: goldengate-fased-read-scheduler-test [scheduler.mlir egress.mlir]");
    for(bool selected:{false,true}) {
      for(unsigned reuse:{1u,2u})for(unsigned flight:{1u,10u,20u})
        behavior(ctx,selected,reuse,flight);
      mapping(ctx,selected);rejection(ctx,selected);
    }
  } catch(const std::exception &e) {llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
