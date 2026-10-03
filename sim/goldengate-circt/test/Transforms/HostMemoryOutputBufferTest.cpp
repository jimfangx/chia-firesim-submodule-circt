// See LICENSE for license details.
#include "goldengate/HostMemoryOutputBuffer.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx){
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto address=token("id: uint<5>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>");
  std::string s="module { firrtl.circuit \"GGHostMemoryReadResponseWrapper\" { firrtl.module @GGHostMemoryReadResponseWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %host_mem: !firrtl.bundle<aw: "+address+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<5>, resp: uint<2>")+", ar: "+address+", r flip: "+token("id: uint<5>, last: uint<1>, data: uint<64>, resp: uint<2>")+">) {} firrtl.module @GGFASEDTokenEngine() {} } }";
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> as;
  for(auto n:{"diagnostic","hostReset","host_mem.aw.bits.addr","host_mem.r.bits.data","host_mem"})
    as.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGHostMemoryReadResponseWrapper|GGHostMemoryReadResponseWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(as));
  auto k=b.getDictionaryAttr({b.getNamedAttr("memoryRegionName",b.getStringAttr("MainMemory_0")),
    b.getNamedAttr("axi4Widths",b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),
      b.getNamedAttr("dataBits",b.getI64IntegerAttr(64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))})),
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("maxReadTransfer",b.getI64IntegerAttr(8)), b.getNamedAttr("idReuse",b.getI64IntegerAttr(1)), b.getNamedAttr("maxFlight",b.getI64IntegerAttr(10))}))});
  top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k);
  auto wrapper=top(c,"GGHostMemoryReadResponseWrapper");
  auto master=cast<BundleType>(wrapper.getPorts()[3].type);
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto inner=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadWrapper"),
    ConventionAttr::get(&ctx,Convention::Internal),wrapper.getPorts());
  auto readType=master.getElements()[4].type;
  SmallVector<PortInfo> routerPorts{{b.getStringAttr("host"),readType,Direction::In},
    {b.getStringAttr("unused1"),UIntType::get(&ctx,1,false),Direction::Out},
    {b.getStringAttr("unused2"),UIntType::get(&ctx,1,false),Direction::Out}};
  auto router=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadResponseRouter"),
    ConventionAttr::get(&ctx,Convention::Internal),routerPorts);
  b.setInsertionPointToStart(wrapper.getBodyBlock());
  auto sim=b.create<InstanceOp>(c.getLoc(),inner,"sim"),r=b.create<InstanceOp>(c.getLoc(),router,"read_responses");
  auto arg=[&](unsigned i){return wrapper.getBodyBlock()->getArgument(i);};
  auto f=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(c.getLoc(),v,n);};
  for(auto ch:master.getElements()) {
    Value host=f(arg(3),ch.name),child=ch.name=="r"?r.getResult(0):f(sim.getResult(3),ch.name);
    b.create<ConnectOp>(c.getLoc(),ch.isFlip?child:host,ch.isFlip?host:child);
  }
  for(unsigned i:{0u,1u})b.create<ConnectOp>(c.getLoc(),sim.getResult(i),arg(i));
  b.create<ConnectOp>(c.getLoc(),arg(2),sim.getResult(2));
  return root;
}
// Interpreter evaluates the generated hierarchical SSA, including asynchronous
// RAM and instance connections. Prepare every edge before committing any state.
struct Interpreter {
  FModuleOp module;Interpreter *parent=nullptr;InstanceOp instance;
  std::map<std::string,Value> drivers;
  std::map<std::string,APInt> memo,inputs;
  llvm::DenseMap<Value,APInt> state,next;
  std::map<Operation *,std::unique_ptr<Interpreter>> children;
  MemOp ram;std::vector<APInt> memory;
  bool write=false;unsigned writeAddr=0;APInt writeData=APInt(71,0);
  std::string key(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  std::pair<Value,std::string> split(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>()) {auto p=split(f.getInput());p.second+="."+f.getFieldName().str();return p;}
    return {v,""};
  }
  unsigned width(Value v){return *cast<UIntType>(v.getType()).getWidth();}
  Interpreter(FModuleOp m,CircuitOp c,Interpreter *p=nullptr,InstanceOp i={}):module(m),parent(p),instance(i) {
    for(auto connect:m.getOps<StrictConnectOp>())require(drivers.emplace(key(connect.getDest()),connect.getSrc()).second,"multiple drivers");
    for(auto child:m.getOps<InstanceOp>())children[child.getOperation()]=std::make_unique<Interpreter>(top(c,child.getModuleName()),c,this,child);
    for(auto mem:m.getOps<MemOp>()) {require(!ram,"multiple RAMs");ram=mem;}
    if(ram)memory.assign(ram.getDepth(),APInt(*cast<UIntType>(ram.getDataType()).getWidth(),0));
    if(ram)require(ram.getDepth()==2 &&
      ram.getReadLatency()==0&&ram.getWriteLatency()==1&&ram.getRuw()==RUWAttr::Undefined,"queue memory geometry");
  }
  Value arg(unsigned i){return module.getBodyBlock()->getArgument(i);}
  void clear(){memo.clear();for(auto &p:children)p.second->clear();}
  APInt at(Value root,StringRef path,unsigned w) {
    std::string k=key(root)+path.str();if(memo.count(k))return memo.at(k);
    APInt n(w,0);
    if(drivers.count(k))n=eval(drivers.at(k));
    else if(auto a=dyn_cast<BlockArgument>(root)) {
      if(parent)n=parent->at(instance.getResult(a.getArgNumber()),path,w);
      else n=inputs.at(module.getPortName(a.getArgNumber()).str()+path.str());
    } else if(auto i=root.getDefiningOp<InstanceOp>()) {
      auto number=cast<OpResult>(root).getResultNumber();n=children.at(i.getOperation())->at(children.at(i.getOperation())->arg(number),path,w);
    } else if(ram&&root==ram.getResult(0)&&path==".data")
      n=memory.at(at(ram.getResult(0),".addr",1).getZExtValue());
    else throw std::runtime_error("missing hierarchical driver "+k);
    n=n.zextOrTrunc(w);memo.insert_or_assign(k,n);return n;
  }
  APInt eval(Value v) {
    auto k=key(v);if(memo.count(k))return memo.at(k);auto *op=v.getDefiningOp();unsigned w=width(v);APInt n(w,0);
    auto x=[&](unsigned i){return eval(op->getOperand(i));};
    if(isa_and_nonnull<RegOp,RegResetOp>(op)) {if(state.count(v))n=state.lookup(v);}
    else if(drivers.count(k))n=eval(drivers.at(k));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue();
    else if(isa_and_nonnull<AndPrimOp>(op))n=x(0)&x(1);
    else if(isa_and_nonnull<OrPrimOp>(op))n=x(0)|x(1);
    else if(isa_and_nonnull<XorPrimOp>(op))n=x(0)^x(1);
    else if(isa_and_nonnull<NotPrimOp>(op))n=~x(0);
    else if(isa_and_nonnull<EQPrimOp>(op))n=APInt(1,x(0)==x(1));
    else if(isa_and_nonnull<NEQPrimOp>(op))n=APInt(1,x(0)!=x(1));
    else if(isa_and_nonnull<AddPrimOp>(op))n=x(0).zextOrTrunc(w)+x(1).zextOrTrunc(w);
    else if(isa_and_nonnull<SubPrimOp>(op))n=x(0).zextOrTrunc(w)-x(1).zextOrTrunc(w);
    else if(isa_and_nonnull<MuxPrimOp>(op))n=x(x(0).isZero()?2:1);
    else if(isa_and_nonnull<CatPrimOp>(op)){auto a=x(0),z=x(1);n=(a.zext(w)<<z.getBitWidth())|z.zext(w);}
    else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op))n=x(0).lshr(bits.getLo()).trunc(w);
    else {auto p=split(v);n=at(p.first,p.second,w);}
    n=n.zextOrTrunc(w);memo.insert_or_assign(k,n);return n;
  }
  void prepare() {
    next.clear();
    for(auto a:module.getOps<AssertOp>())
      require(eval(a->getOperand(2)).isZero()||!eval(a->getOperand(1)).isZero(),"generated burst count assertion fired");
    for(auto r:module.getOps<RegResetOp>())next[r.getResult()]=eval(r.getResetSignal()).isZero()?eval(drivers.at(key(r.getResult()))):eval(r.getResetValue());
    for(auto r:module.getOps<RegOp>())next[r.getResult()]=eval(drivers.at(key(r.getResult())));
    write=false;
    if(ram) {
      write=!at(ram.getResult(1),".en",1).isZero()&&!at(ram.getResult(1),".mask",1).isZero();
      if(write){writeAddr=at(ram.getResult(1),".addr",1).getZExtValue();writeData=at(ram.getResult(1),".data",memory[0].getBitWidth());}
    }
    for(auto &p:children)p.second->prepare();
  }
  void commit(){state=next;if(write)memory.at(writeAddr)=writeData;for(auto &p:children)p.second->commit();}
  void input(StringRef n,unsigned w,uint64_t v){inputs.insert_or_assign(n.str(),APInt(w,v));}
  uint64_t output(StringRef path,unsigned w){return at(arg(path.starts_with("in")?2:3),path.drop_front(path.starts_with("in")?2:3),w).getZExtValue();}
};
const StringRef queueNames[]{"GGHostMemoryAddressQueue2", "GGHostMemoryWriteQueue2",
  "GGHostMemoryAckQueue2", "GGHostMemoryReadQueue2"};
APInt randomPayload(unsigned w,std::mt19937_64 &rng) {
  APInt value(w,rng());
  if(w>64)value |= APInt(w,rng()) << 64;
  return value;
}
void putPayload(Interpreter &sim,Value port,StringRef prefix,APInt data) {
  auto token=cast<BundleType>(port.getType());auto bits=cast<BundleType>(token.getElements()[2].type);
  unsigned low=0;
  for(auto e:llvm::reverse(bits.getElements())) {
    unsigned w=*cast<UIntType>(e.type).getWidth();
    sim.input(prefix.str()+".bits."+e.name.getValue().str(),w,data.lshr(low).trunc(w).getZExtValue());low+=w;
  }
}
APInt getPayload(Interpreter &sim,Value port,StringRef path) {
  auto bits=cast<BundleType>(cast<BundleType>(port.getType()).getElements()[2].type);
  unsigned width=0;for(auto e:bits.getElements())width+=*cast<UIntType>(e.type).getWidth();
  APInt result(width,0);
  for(auto e:bits.getElements()) {
    unsigned w=*cast<UIntType>(e.type).getWidth();
    result=(result<<w)|sim.at(port,path.str()+".bits."+e.name.getValue().str(),w).zext(width);
  }
  return result;
}
void queueEdges(CircuitOp circuit) {
  std::mt19937_64 rng(241);unsigned cases=0,resetWrites=0,full=0,collisions=0;
  for(auto name:queueNames) {
    Interpreter sim(top(circuit,name),circuit);
    unsigned w=sim.memory[0].getBitWidth();
    // Exhaustive control states include the full+pop and empty+push cases:
    // neither pipe nor flow may grant a same-cycle transfer there.
    for(unsigned sample=0;sample<64;++sample) {
      unsigned ep=sample&1,dp=(sample>>1)&1,mf=(sample>>2)&1;
      bool valid=sample&8,ready=sample&16,reset=sample&32;
      sim.clear();sim.state.clear();
      for(auto r:sim.module.getOps<RegResetOp>())sim.state[r.getResult()]=APInt(1,r.getName()=="enq_ptr_value"?ep:r.getName()=="deq_ptr_value"?dp:mf);
      for(auto &v:sim.memory)v=randomPayload(w,rng);
      auto data=randomPayload(w,rng);putPayload(sim,sim.arg(2),"enq",data);
      sim.input("reset",1,reset);sim.input("enq.valid",1,valid);sim.input("deq.ready",1,ready);
      bool er=!(ep==dp&&mf),dv=!(ep==dp&&!mf),push=er&&valid,pop=dv&&ready;
      require(sim.at(sim.arg(2),".ready",1).getBoolValue()==er&&sim.at(sim.arg(3),".valid",1).getBoolValue()==dv,"no-flow/no-pipe handshake mismatch");
      require(getPayload(sim,sim.arg(3),"")==sim.memory[dp],"asynchronous payload mismatch");
      auto expected=sim.memory;if(push)expected[ep]=data;
      sim.prepare();sim.commit();require(sim.memory==expected,"RAM write including reset mismatch");
      for(auto r:sim.module.getOps<RegResetOp>()) {
        unsigned v=r.getName()=="enq_ptr_value"?(ep+push)%2:r.getName()=="deq_ptr_value"?(dp+pop)%2:push!=pop?push:mf;
        require(sim.state.lookup(r.getResult()).getZExtValue()==(reset?0:v),"occupancy reset/next state mismatch");
      }
      ++cases;resetWrites+=reset&&push;full+=!er;collisions+=push&&ep==dp;
    }
  }
  require(resetWrites&&full&&collisions,"missing exhaustive coverage");
  llvm::outs()<<cases<<" exhaustive queue states, reset writes "<<resetWrites<<", full "<<full<<", collisions "<<collisions<<"\n";
}
void behavior(CircuitOp circuit) {
  Interpreter sim(top(circuit,"GGHostMemoryOutputBuffer"),circuit);
  std::mt19937_64 rng(242);std::array<std::deque<APInt>,5> reference;
  const StringRef channels[]{"aw","w","b","ar","r"};
  unsigned pushes=0,pops=0,stalls=0,full=0,simultaneous=0,resets=0;
  for(unsigned cycle=0;cycle<6000;++cycle) {
    bool reset=cycle%137==0;sim.clear();sim.input("reset",1,reset);
    for(unsigned j=0;j<5;++j) {
      bool request=j!=2&&j!=4;StringRef producer=request?"in":"out",consumer=request?"out":"in";
      auto channel=cast<BundleType>(sim.arg(2).getType()).getElements()[j];
      auto token=cast<BundleType>(channel.type);auto bits=cast<BundleType>(token.getElements()[2].type);
      unsigned w=0;for(auto e:bits.getElements())w+=*cast<UIntType>(e.type).getWidth();
      auto data=randomPayload(w,rng);bool valid=rng()%4!=0,ready=rng()%3!=0;
      sim.input(producer.str()+"."+channels[j].str()+".valid",1,valid);
      sim.input(consumer.str()+"."+channels[j].str()+".ready",1,ready);
      // Populate fields using the channel type, not the whole master port.
      unsigned low=0;for(auto e:llvm::reverse(bits.getElements())) {
        unsigned bw=*cast<UIntType>(e.type).getWidth();sim.input(producer.str()+"."+channels[j].str()+".bits."+e.name.getValue().str(),bw,data.lshr(low).trunc(bw).getZExtValue());low+=bw;
      }
      bool er=reference[j].size()!=2,dv=!reference[j].empty();
      require(sim.output(producer.str()+"."+channels[j].str()+".ready",1)==er,"channel ready mismatch");
      require(sim.output(consumer.str()+"."+channels[j].str()+".valid",1)==dv,"channel valid mismatch");
      if(dv) {
        APInt actual(w,0);for(auto e:bits.getElements()) {
          unsigned bw=*cast<UIntType>(e.type).getWidth();actual=(actual<<bw)|APInt(w,sim.output(consumer.str()+"."+channels[j].str()+".bits."+e.name.getValue().str(),bw));
        }
        require(actual==reference[j].front(),"channel payload order/stall mismatch");
      }
      bool push=valid&&er,pop=ready&&dv;
      if(pop)reference[j].pop_front();if(push)reference[j].push_back(data);
      if(reset)reference[j].clear();
      pushes+=push;pops+=pop;stalls+=dv&&!ready;full+=!er;simultaneous+=push&&pop;
    }
    sim.prepare();sim.commit();resets+=reset;
  }
  require(pushes&&pops&&stalls&&full&&simultaneous&&resets,"missing traffic coverage");
  llvm::outs()<<"6000 five-channel hierarchical SSA edges; accepted "<<pushes<<"/"<<pops<<", stalls "<<stalls<<", full "<<full<<", simultaneous "<<simultaneous<<", resets "<<resets<<"\n";
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto root=fixture(ctx);auto circuit=*root->getOps<CircuitOp>().begin();
    auto wrapper=top(circuit,"GGHostMemoryReadResponseWrapper");auto annotations=circuit->getAttr("rawAnnotations");
    std::map<std::string,std::string> before;for(auto m:circuit.getOps<FModuleOp>())if(m!=wrapper)before[m.getName().str()]=dump(m);
    require(succeeded(goldengate::addHostMemoryOutputBuffer(circuit,error)),error);
    require(succeeded(verify(*root)),"invalid output buffer IR");
    require(circuit->getAttr("rawAnnotations")==annotations&&wrapper.getNumPorts()==4,"top ports or annotations changed");
    for(auto p:before)require(dump(top(circuit,p.first))==p.second,"existing inner module changed");
    auto helper=top(circuit,"GGHostMemoryOutputBuffer");
    require(std::distance(helper.getOps<InstanceOp>().begin(),helper.getOps<InstanceOp>().end())==5,"five queues missing");
    queueEdges(circuit);behavior(circuit);
    auto bindings=[&](FModuleOp t) {
      InstanceOp buffer;for(auto i:t.getOps<InstanceOp>())if(i.getName()=="host_memory_buffer")buffer=i;
      require(bool(buffer),"missing buffer instance");unsigned out=0,clock=0,reset=0;
      Value host;for(auto [i,p]:llvm::enumerate(t.getPorts()))if(p.name=="host_mem")host=t.getBodyBlock()->getArgument(i);
      for(auto c:t.getOps<ConnectOp>())out+=c.getDest()==host&&c.getSrc()==buffer.getResult(3);
      for(auto c:t.getOps<StrictConnectOp>()) {
        auto arg=dyn_cast<BlockArgument>(c.getSrc());
        clock+=c.getDest()==buffer.getResult(0)&&arg&&t.getPortName(arg.getArgNumber())=="hostClock";
        reset+=c.getDest()==buffer.getResult(1)&&arg&&t.getPortName(arg.getArgNumber())=="hostReset";
      }
      std::map<std::string,unsigned> in;
      for(auto f:t.getOps<SubfieldOp>())if(f.getInput()==buffer.getResult(2))++in[f.getFieldName().str()];
      require(out==1&&clock==1&&reset==1&&in.size()==5,"buffer boundary binding mismatch");
      for(auto n:{"aw","w","b","ar","r"})require(in[n]==1,"channel omitted or duplicated");
    };
    bindings(wrapper);
    for(unsigned bad=0;bad<9;++bad) {
      auto test=fixture(ctx);auto c=*test->getOps<CircuitOp>().begin();auto t=top(c,"GGHostMemoryReadResponseWrapper");OpBuilder b(&ctx);
      if(bad==0)c->removeAttr("rawAnnotations");
      else if(bad==1)c.setName("Wrong");
      else if(bad==2){for(auto x:t.getOps<ConnectOp>())if(x.getDest().getDefiningOp<SubfieldOp>()){x.erase();break;}}
      else if(bad==3){b.setInsertionPointToStart(t.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top(c,"GGHostMemoryReadWrapper"),"duplicate");}
      else if(bad==4){auto e=top(c,"GGFASEDTokenEngine");auto key=e->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");NamedAttrList w(key.getAs<DictionaryAttr>("axi4Widths"));w.set("idBits",b.getI64IntegerAttr(99));NamedAttrList k(key);k.set("axi4Widths",w.getDictionary(&ctx));e->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));}
      else if(bad==5){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(queueNames[0]),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
      else if(bad==6){b.setInsertionPointToStart(t.getBodyBlock());b.create<SubfieldOp>(c.getLoc(),t.getBodyBlock()->getArgument(3),"aw");}
      else if(bad==7){for(auto x:t.getOps<ConnectOp>())if(x.getSrc()==t.getBodyBlock()->getArgument(0)){x.erase();break;}}
      else {b.setInsertionPointToEnd(c.getBodyBlock());auto user=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("User"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});b.setInsertionPointToStart(user.getBodyBlock());b.create<InstanceOp>(c.getLoc(),t,"nested");}
      auto s=dump(*test);require(failed(goldengate::addHostMemoryOutputBuffer(c,error))&&!error.empty(),"invalid boundary accepted");require(s==dump(*test),"rejection mutated IR");
    }
    auto s=dump(*root);require(failed(goldengate::addHostMemoryOutputBuffer(circuit,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"10 atomic rejections; eight buffer boundary bindings and annotations preserved\n";
    if(argc>=2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"cannot write boundary");os<<"module { firrtl.circuit \"GGHostMemoryOutputBuffer\" {\n";for(auto n:queueNames){top(circuit,n)->print(os);os<<"\n";}helper->print(os);os<<"\n} }\n";}
    if(argc>=3) {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real boundary parse failed");auto c=*real->getOps<CircuitOp>().begin();
      auto t=top(c,"GGHostMemoryReadResponseWrapper");auto ports=t.getPorts();auto ann=c->getAttr("rawAnnotations");
      require(succeeded(goldengate::addHostMemoryOutputBuffer(c,error)),error);require(succeeded(verify(*real)),"real boundary invalid");
      require(t.getNumPorts()==ports.size()&&c->getAttr("rawAnnotations")==ann,"real ports/annotations changed");for(auto [i,p]:llvm::enumerate(ports)) {auto q=t.getPorts()[i];require(p.name==q.name&&p.type==q.type&&p.direction==q.direction&&p.annotations==q.annotations&&p.sym==q.sym,"real port identity changed");}
      bindings(t);
      llvm::outs()<<"Real Rocket boundary: "<<t.getNumPorts()<<" ports, "<<cast<ArrayAttr>(ann).size()<<" annotations\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
