// See LICENSE for license details.
#include "goldengate/FASEDReadDeinterleaver.h"
#include "goldengate/FASEDAddressTranslation.h"
#include "circt/Dialect/HW/HWDialect.h"
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
  auto address=token("id: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>");
  std::string s="module { firrtl.circuit \"GGFASEDHostMemoryWrapper\" { firrtl.module @GGFASEDHostMemoryWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_mem: !firrtl.bundle<aw: "+address+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<4>, resp: uint<2>")+", ar: "+address+", r flip: "+token("id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+">) {} firrtl.module @GGFASEDTokenEngine() {} } }";
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> as;
  for(auto n:{"diagnostic","hostReset","fased_host_mem.aw.bits.addr","fased_host_mem.r.bits.data","fased_host_mem"})
    as.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),
      b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDHostMemoryWrapper|GGFASEDHostMemoryWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(as));
  SmallVector<Attribute> sets;
  const uint64_t base[]{0x80000000ULL,0x100000000ULL,0x200000000ULL,0x400000000ULL};
  const uint64_t mask[]{0x7fffffffULL,0xffffffffULL,0x1ffffffffULL,0x7fffffffULL};
  for(unsigned j=0;j<4;++j)sets.push_back(b.getDictionaryAttr({
    b.getNamedAttr("base",b.getI64IntegerAttr(base[j])),
    b.getNamedAttr("mask",b.getI64IntegerAttr(mask[j]))}));
  auto k=b.getDictionaryAttr({b.getNamedAttr("memoryRegionName",b.getStringAttr("MainMemory_0")),
    b.getNamedAttr("axi4Widths",b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),
      b.getNamedAttr("dataBits",b.getI64IntegerAttr(64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))})),
    b.getNamedAttr("axi4Edge",b.getDictionaryAttr({b.getNamedAttr("address",b.getArrayAttr(sets)), b.getNamedAttr("maxReadTransfer",b.getI64IntegerAttr(8)), b.getNamedAttr("idReuse",b.getI64IntegerAttr(1)), b.getNamedAttr("maxFlight",b.getI64IntegerAttr(10))}))});
  top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k);
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
  MemOp ram;std::array<APInt,8> memory;
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
    for(auto &x:memory)x=APInt(71,0);
    if(ram)require(ram.getDepth()==8 && ram.getDataType()==UIntType::get(m.getContext(),71,false)&&
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
      n=memory.at(at(ram.getResult(0),".addr",3).getZExtValue());
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
      if(write){writeAddr=at(ram.getResult(1),".addr",3).getZExtValue();writeData=at(ram.getResult(1),".data",71);}
    }
    for(auto &p:children)p.second->prepare();
  }
  void commit(){state=next;if(write)memory.at(writeAddr)=writeData;for(auto &p:children)p.second->commit();}
  void input(StringRef n,unsigned w,uint64_t v){inputs.insert_or_assign(n.str(),APInt(w,v));}
  uint64_t output(StringRef path,unsigned w){return at(arg(path.starts_with("in")?2:3),path.drop_front(path.starts_with("in")?2:3),w).getZExtValue();}
};
void queueEdges(CircuitOp circuit) {
  Interpreter sim(top(circuit,"GGFASEDDeinterleaveQueue8"),circuit);
  std::mt19937_64 rng(241);unsigned resetWrites=0,full=0,wrap=0,collisions=0;
  for(unsigned sample=0;sample<2048;++sample) {
    unsigned ep=rng()%8,dp=rng()%8,mf=rng()%2;
    bool valid=rng()%2,ready=rng()%2,reset=sample%7==0;
    uint64_t data=rng();unsigned id=rng()%16,resp=rng()%4,last=rng()%2;
    sim.clear();sim.state.clear();
    for(auto r:sim.module.getOps<RegResetOp>()) {
      unsigned v=r.getName()=="enq_ptr_value"?ep:r.getName()=="deq_ptr_value"?dp:mf;
      sim.state[r.getResult()]=APInt(sim.width(r.getResult()),v);
    }
    for(auto &v:sim.memory)v=(APInt(71,rng()%128)<<64)|APInt(71,rng());
    sim.input("reset",1,reset);sim.input("enq.valid",1,valid);sim.input("deq.ready",1,ready);
    sim.input("enq.bits.id",4,id);sim.input("enq.bits.data",64,data);
    sim.input("enq.bits.resp",2,resp);sim.input("enq.bits.last",1,last);
    bool er=!(ep==dp&&mf),dv=!(ep==dp&&!mf),push=valid&&er,pop=ready&&dv;
    require(sim.at(sim.arg(2),".ready",1).getBoolValue()==er&&sim.at(sim.arg(3),".valid",1).getBoolValue()==dv,"Queue_34 no-flow/no-pipe handshake");
    auto read=sim.memory[dp];
    require(sim.at(sim.arg(3),".bits.id",4)==read.lshr(67).trunc(4)&&
      sim.at(sim.arg(3),".bits.data",64)==read.lshr(3).trunc(64)&&
      sim.at(sim.arg(3),".bits.resp",2)==read.lshr(1).trunc(2)&&
      sim.at(sim.arg(3),".bits.last",1)==read.trunc(1),"Queue_34 asynchronous RAM unpack");
    auto expected=sim.memory;
    if(push)expected[ep]=(APInt(71,id)<<67)|(APInt(71,data)<<3)|APInt(71,(resp<<1)|last);
    sim.prepare();sim.commit();require(sim.memory==expected,"Queue_34 accepted RAM write including reset");
    for(auto r:sim.module.getOps<RegResetOp>()) {
      unsigned v=r.getName()=="enq_ptr_value"?(ep+push)%8:r.getName()=="deq_ptr_value"?(dp+pop)%8:push!=pop?push:mf;
      require(sim.state.lookup(r.getResult()).getZExtValue()==(reset?0:v),"Queue_34 pointer/full next state");
    }
    resetWrites+=reset&&push;full+=!er;wrap+=(pop&&dp==7)||(push&&ep==7);collisions+=push&&ep==dp;
  }
  require(resetWrites&&full&&wrap&&collisions,"queue sample coverage missing");
  llvm::outs()<<"2048 queue state samples; reset writes "<<resetWrites<<", full "<<full<<", wraps "<<wrap<<", RAM collisions "<<collisions<<"\n";
}
struct Beat {unsigned id,resp;uint64_t data;bool last;};
void behavior(CircuitOp circuit) {
  Interpreter sim(top(circuit,"GGFASEDReadDeinterleaver"),circuit);
  std::array<std::deque<Beat>,16> queues;
  std::array<unsigned,16> complete{},remaining{};
  bool locked=false;unsigned id=0;std::mt19937_64 rng(240);
  unsigned cycles=0,pushes=0,pops=0,stalls=0,full=0,simultaneous=0,resets=0;
  auto step=[&](bool valid,Beat beat,bool ready,bool reset=false){
    sim.clear();sim.input("reset",1,reset);sim.input("in.r.ready",1,ready);
    sim.input("out.r.valid",1,valid);sim.input("out.r.bits.id",4,beat.id);
    sim.input("out.r.bits.data",64,beat.data);sim.input("out.r.bits.resp",2,beat.resp);sim.input("out.r.bits.last",1,beat.last);
    bool er=queues[beat.id].size()<8,push=valid&&er,pop=locked&&ready,done=pop&&queues[id].front().last;
    require(sim.output("out.r.ready",1)==er,"FIFO full/ready differs");
    require(sim.output("in.r.valid",1)==locked,"complete-burst output validity differs");
    if(locked) {
      auto b=queues[id].front();
      require(sim.output("in.r.bits.id",4)==b.id&&sim.output("in.r.bits.resp",2)==b.resp&&
        sim.output("in.r.bits.data",64)==b.data&&sim.output("in.r.bits.last",1)==b.last,"response ID/data/resp/last differs");
    }
    sim.prepare();sim.commit();
    if(pop){queues[id].pop_front();if(done)--complete[id];}
    if(push){queues[beat.id].push_back(beat);if(beat.last)++complete[beat.id];}
    // Golden pending vector uses next counts, including both accepted edges.
    if(!locked||done){locked=false;id=0;for(unsigned j=0;j<16;++j)if(complete[j]){locked=true;id=j;break;}}
    if(reset){for(auto &q:queues)q.clear();complete.fill(0);locked=false;++resets;}
    ++cycles;pushes+=push;pops+=pop;stalls+=locked&&!ready;full+=!er;simultaneous+=push&&done;
    return push;
  };
  step(false,{0,0,0,false},false,true);
  // All ID queues filled to eight beats, partial bursts cannot be selected.
  for(unsigned beat=0;beat<8;++beat)for(unsigned j=0;j<16;++j)
    step(true,{j,j%4,(uint64_t(j)<<32)|beat,beat==7},false);
  for(unsigned j=0;j<16;++j)step(true,{j,3,99,true},false); // full rejects
  for(unsigned j=0;j<128;++j)step(false,{0,0,0,false},true);
  for(unsigned cycle=0;cycle<8000;++cycle) {
    unsigned j=rng()%16;if(!remaining[j])remaining[j]=1+rng()%8;
    bool valid=rng()%4!=0,ready=rng()%3!=0,reset=cycle%197==0;
    bool accepted=step(valid,{j,unsigned(rng()%4),rng(),remaining[j]==1},ready,reset);
    if(accepted)--remaining[j];if(reset)remaining.fill(0);
  }
  // Drain partial bursts before drain-all to avoid an incomplete-burst deadlock.
  for(unsigned j=0;j<16;++j)while(remaining[j]) {
    if(step(true,{j,j%4,rng(),remaining[j]==1},true))--remaining[j];
  }
  unsigned drain=0;while(locked&&drain++<200)step(false,{0,0,0,false},true);
  require(!locked&&pushes&&pops&&stalls&&full&&simultaneous&&resets,"missing behavioral coverage");
  llvm::outs()<<cycles<<" hierarchical SSA edges; accepted "<<pushes<<"/"<<pops
    <<", stalls "<<stalls<<", full "<<full<<", simultaneous last retirement/enqueue "<<simultaneous<<", resets "<<resets<<"\n";
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    auto root=fixture(ctx);auto circuit=*root->getOps<CircuitOp>().begin();std::string error;
    require(succeeded(goldengate::addFASEDAddressTranslation(circuit,error)),error);
    auto wrapper=top(circuit,"GGFASEDAddressTranslationWrapper");auto annotations=circuit->getAttr("rawAnnotations");
    std::map<std::string,std::string> before;for(auto m:circuit.getOps<FModuleOp>())if(m!=wrapper)before[m.getName().str()]=dump(m);
    require(succeeded(goldengate::addFASEDReadDeinterleaver(circuit,error)),error);
    require(succeeded(verify(*root)),"invalid deinterleaver IR");
    require(circuit->getAttr("rawAnnotations")==annotations&&wrapper.getNumPorts()==4,"top ports or annotations changed");
    for(auto p:before)require(dump(top(circuit,p.first))==p.second,"existing module changed");
    auto helper=top(circuit,"GGFASEDReadDeinterleaver");
    require(std::distance(helper.getOps<InstanceOp>().begin(),helper.getOps<InstanceOp>().end())==16,"sixteen queues missing");
    require(std::distance(helper.getOps<AssertOp>().begin(),helper.getOps<AssertOp>().end())==32,"bounds assertions missing");
    queueEdges(circuit);behavior(circuit);
    for(unsigned bad=0;bad<10;++bad) {
      auto test=fixture(ctx);auto c=*test->getOps<CircuitOp>().begin();require(succeeded(goldengate::addFASEDAddressTranslation(c,error)),error);
      auto t=top(c,"GGFASEDAddressTranslationWrapper");OpBuilder b(&ctx);
      if(bad<6){auto e=top(c,"GGFASEDTokenEngine");auto key=e->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");
        bool width=bad<3;StringRef ns[]{"addrBits","dataBits","idBits","maxReadTransfer","idReuse","maxFlight"};
        StringRef dict=width?"axi4Widths":"axi4Edge";NamedAttrList d(key.getAs<DictionaryAttr>(dict));d.set(ns[bad],b.getI64IntegerAttr(99));
        NamedAttrList k(key);k.set(dict,d.getDictionary(&ctx));e->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));
      } else if(bad==6)c->removeAttr("rawAnnotations");
      else if(bad==7){for(auto x:t.getOps<ConnectOp>())if(auto i=x.getDest().getDefiningOp<InstanceOp>())if(i.getName()=="translation"&&cast<OpResult>(x.getDest()).getResultNumber()==2){x.erase();break;}}
      else if(bad==8){b.setInsertionPointToStart(t.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top(c,"GGFASEDHostMemoryWrapper"),"duplicate");}
      else c.setName("Wrong");
      auto s=dump(*test);require(failed(goldengate::addFASEDReadDeinterleaver(c,error))&&!error.empty(),"invalid boundary accepted");require(s==dump(*test),"rejection mutated IR");
    }
    auto s=dump(*root);require(failed(goldengate::addFASEDReadDeinterleaver(circuit,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"11 atomic rejection cases; all old modules/annotations preserved\n";
    if(argc==2){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"cannot write boundary");os<<"module { firrtl.circuit \"GGFASEDReadDeinterleaver\" {\n";top(circuit,"GGFASEDDeinterleaveQueue8")->print(os);os<<"\n";helper->print(os);os<<"\n} }\n";}
    return 0;
  } catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
