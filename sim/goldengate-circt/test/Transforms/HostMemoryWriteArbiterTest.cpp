// See LICENSE for license details.
#include "goldengate/HostMemoryWriteArbiter.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto addr=token("id: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>");
  auto mem="!firrtl.bundle<aw: "+addr+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<4>, resp: uint<2>")+", ar: "+addr+", r flip: "+token("id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+">";
  std::string ports="in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_mem: "+mem;
  for(auto [n,w]:std::vector<std::pair<std::string,unsigned>>{{"aw_ready",1},{"aw_valid",1},{"aw_bits_addr",34},{"aw_bits_len",8},{"w_ready",1},{"w_valid",1},{"w_bits_data",64},{"w_bits_last",1}})
    ports+=", "+std::string(n.find("ready")!=std::string::npos?"in":"out")+" %loadmem_mem_"+n+": !firrtl.uint<"+std::to_string(w)+">";
  std::string s="module { firrtl.circuit \"GGFASEDAddressTranslationWrapper\" { firrtl.module @GGFASEDAddressTranslationWrapper("+ports+") {} firrtl.module @GGFASEDTokenEngine() {} firrtl.module @GGFASEDHostMemoryBuffer(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>, in %in: "+mem+", out %out: "+mem+") {} firrtl.module @UnusedFixture() {} } }";
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse");
  auto circuit=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto inner=top(circuit,"GGFASEDAddressTranslationWrapper");
  b.setInsertionPointToStart(inner.getBodyBlock());
  auto buf=b.create<InstanceOp>(inner.getLoc(),top(circuit,"GGFASEDHostMemoryBuffer"),"memory_buffer");
  b.create<ConnectOp>(inner.getLoc(),inner.getBodyBlock()->getArgument(3),buf.getResult(3));
  b.create<StrictConnectOp>(inner.getLoc(),buf.getResult(0),inner.getBodyBlock()->getArgument(0));
  b.create<StrictConnectOp>(inner.getLoc(),buf.getResult(1),inner.getBodyBlock()->getArgument(1));
  for(auto n:{"sim","translation","deinterleaver"})b.create<InstanceOp>(inner.getLoc(),top(circuit,"UnusedFixture"),n);
  auto widths=b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))});
  auto edge=b.getDictionaryAttr({b.getNamedAttr("maxReadTransfer",b.getI64IntegerAttr(8)),b.getNamedAttr("idReuse",b.getI64IntegerAttr(1)),b.getNamedAttr("maxFlight",b.getI64IntegerAttr(10))});
  top(circuit,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",widths),b.getNamedAttr("axi4Edge",edge)}));
  SmallVector<Attribute> ann;
  for(auto n:{"diagnostic","loadmem_mem_aw_bits_addr","fased_host_mem.aw.bits.id","fased_host_mem.r.bits.id","fased_host_mem"})
    ann.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~GGFASEDAddressTranslationWrapper|GGFASEDAddressTranslationWrapper>"+std::string(n)))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(ann));return root;
}
Value namedReg(FModuleOp m,StringRef n){for(auto r:m.getOps<RegResetOp>())if(r.getName()==n)return r.getResult();throw std::runtime_error("missing register");}
void exhaustiveQueue(CircuitOp c) {
  auto q=top(c,"GGHostMemoryWriteSourceQueue2");Interpreter sim(q,c);unsigned cases=0,writes=0,bypasses=0;
  for(unsigned code=0;code<256;++code) {
    bool ep=code&1,dp=code&2,mf=code&4,ev=code&8,dr=code&16,rst=code&32;
    unsigned payload=code>>6;
    sim.state[namedReg(q,"enq_ptr_value")]=APInt(1,ep);sim.state[namedReg(q,"deq_ptr_value")]=APInt(1,dp);sim.state[namedReg(q,"maybe_full")]=APInt(1,mf);
    sim.memory={APInt(2,1),APInt(2,2)};sim.input("reset",1,rst);sim.input("enq.valid",1,ev);sim.input("enq.bits",2,payload);sim.input("deq.ready",1,dr);sim.clear();
    bool empty=ep==dp&&!mf,full=ep==dp&&mf,valid=ev||!empty,push=!full&&ev&&!(empty&&dr),pop=valid&&dr&&!empty;
    require(sim.at(sim.arg(2),".ready",1).getBoolValue()==!full,"queue ready");
    require(sim.at(sim.arg(3),".valid",1).getBoolValue()==valid,"queue valid");
    require(sim.at(sim.arg(3),".bits",2).getZExtValue()==(empty?payload:dp?2:1),"queue data/bypass");
    sim.prepare();require(sim.write==push,"queue RAM write (including reset)");sim.commit();
    require(sim.state.lookup(namedReg(q,"enq_ptr_value")).getBoolValue()==(!rst&&(ep^push)),"queue next enq");
    require(sim.state.lookup(namedReg(q,"deq_ptr_value")).getBoolValue()==(!rst&&(dp^pop)),"queue next deq");
    require(sim.state.lookup(namedReg(q,"maybe_full")).getBoolValue()==(!rst&&((push!=pop)?push:mf)),"queue next occupancy");
    ++cases;writes+=rst&&push;bypasses+=empty&&ev&&dr;
  }
  llvm::outs()<<"Queue_21 comparison: "<<cases<<" exhaustive cases, "<<writes<<" reset RAM writes, "<<bypasses<<" bypasses\n";
}
// Reference equations are the immutable AXI4Xbar/Queue_21 expressions, including
// mask rotation on idle selection, regardless of downstream AW acceptance.
struct Oracle {
  bool idle=true,s0=false,s1=false,latched=false,ep=false,dp=false,mf=false;
  unsigned mask=3;std::array<unsigned,2> ram{0,0};
};
void trace(CircuitOp c) {
  auto helper=top(c,"GGHostMemoryWriteArbiter");Interpreter sim(helper,c);Oracle o;std::mt19937_64 rng(242);
  unsigned fulls=0,stalls=0,reservations=0,bypasses=0,simultaneous=0,resets=0,checks=0;
  const std::vector<std::pair<std::string,unsigned>> af{{"id",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",34}};
  auto set=[&](std::string n,unsigned w,uint64_t v){sim.input(n,w,v);};
  // Synchronous reset first, then 12000 arbitrary stall and source-valid edges.
  for(unsigned t=0;t<12001;++t) {
    bool rst=t==0||t%233==0;resets+=rst;
    bool v0=rng()%4!=0,v1=rng()%4!=0,ar=rng()%3!=0,wr=rng()%3!=0;
    bool wv0=rng()%3!=0,wv1=rng()%3!=0,l0=rng()%4==0,l1=rng()%4==0;
    if(t==1){v0=v1=true;ar=wr=false;} // Both valid after reset must select FASED.
    std::map<std::string,uint64_t> p0,p1;
    for(auto [n,w]:af){p1[n]=rng()&((uint64_t(1)<<w)-1);p0[n]=n=="addr"||n=="len"?rng()&((uint64_t(1)<<w)-1):n=="id"?16:n=="size"?3:n=="burst"?1:0;set("fased.aw.bits."+n,w,p1[n]);}
    for(auto n:{"addr","len"})set("load.aw.bits."+std::string(n),n==std::string("addr")?34:8,p0[n]);
    uint64_t data0=rng(),data1=rng();unsigned strb=rng()&255;
    set("reset",1,rst);set("load.aw.valid",1,v0);set("fased.aw.valid",1,v1);set("out.aw.ready",1,ar);set("out.w.ready",1,wr);
    set("load.w.valid",1,wv0);set("fased.w.valid",1,wv1);set("load.w.bits.last",1,l0);set("fased.w.bits.last",1,l1);
    set("load.w.bits.data",64,data0);set("fased.w.bits.data",64,data1);set("fased.w.bits.strb",8,strb);sim.clear();
    // Interpreter starts at all-zero uninitialized registers; first edge is
    // reset. Establish golden reset state for combinational pre-reset checks.
    if(t==0){for(auto r:helper.getOps<RegResetOp>())sim.state[r.getResult()]=sim.eval(r.getResetValue());}
    unsigned vs=(unsigned(v1)<<1)|v0,filter=((vs&(~o.mask&3))<<2)|vs;
    unsigned unready=((filter|(filter>>1))>>1)|(o.mask<<2);
    unsigned readys=(~((unready>>2)&(unready&3)))&3,winner=readys&vs;
    bool select0=o.idle?(winner&1):o.s0,select1=o.idle?(winner&2):o.s1;
    bool empty=o.ep==o.dp&&!o.mf,full=o.ep==o.dp&&o.mf;
    bool av=o.idle?(v0||v1):((o.s0&&v0)||(o.s1&&v1));
    bool admitted=o.latched||!full,afire=av&&ar&&admitted,qv=av&&!o.latched;
    unsigned source=empty?(unsigned(select1)<<1)|select0:o.ram[o.dp];
    bool d0=source&1,d1=source&2,dv=qv||!empty;
    bool wv=(d0&&wv0)||(d1&&wv1),last=(d0&&l0)||(d1&&l1),dr=wv&&last&&wr;
    auto check=[&](unsigned port,StringRef path,unsigned w,uint64_t want){require(sim.at(sim.arg(port),path,w).getZExtValue()==want,"AXI4Xbar mismatch at "+path.str()+" cycle "+std::to_string(t));++checks;};
    check(2,".aw.ready",1,ar&&admitted&&(o.idle?(readys&1):o.s0));
    check(3,".aw.ready",1,ar&&admitted&&(o.idle?(readys&2):o.s1));check(4,".aw.valid",1,av&&admitted);
    for(auto [n,w]:af)check(4,".aw.bits."+n,n=="id"?5:w,(select0?p0[n]:0)|(select1?p1[n]:0));
    check(2,".w.ready",1,wr&&dv&&d0);check(3,".w.ready",1,wr&&dv&&d1);check(4,".w.valid",1,wv&&dv);
    check(4,".w.bits.last",1,last);check(4,".w.bits.strb",8,(d0?255:0)|(d1?strb:0));check(4,".w.bits.data",64,(d0?data0:0)|(d1?data1:0));
    if(t==1)require(sim.at(sim.arg(4),".aw.bits.id",5).getZExtValue()==p1["id"],"reset priority must favor FASED");
    bool push=!full&&qv&&!(empty&&dr),pop=dv&&dr&&!empty;
    fulls+=full;stalls+=av&&!afire;reservations+=qv&&!full&&!afire;bypasses+=empty&&qv&&dr;simultaneous+=push&&pop;
    sim.prepare();sim.commit();
    if(push)o.ram[o.ep]=(unsigned(select1)<<1)|select0; // RAM writes survive reset.
    if(rst){auto ram=o.ram;o=Oracle();o.ram=ram;}
    else {
      if(o.idle&&vs)o.mask=winner|((winner<<1)&3);
      o.s0=select0;o.s1=select1;o.idle=afire?true:vs?false:o.idle;
      o.latched=afire?false:o.latched||(!full&&qv);
      o.ep^=push;o.dp^=pop;if(push!=pop)o.mf=push;
    }
  }
  require(fulls&&stalls&&reservations&&bypasses&&simultaneous&&resets,"missing trace coverage");
  llvm::outs()<<"AXI4Xbar comparison: "<<checks<<" leaf checks / 12001 edges; full="<<fulls<<", stalls="<<stalls<<", reservations="<<reservations<<", bypass="<<bypasses<<", simultaneous="<<simultaneous<<", reset="<<resets<<"\n";
}
void rejected(MLIRContext &ctx,unsigned mode) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();auto m=top(c,"GGFASEDAddressTranslationWrapper");OpBuilder b(&ctx);
  if(mode==0)c.setNameAttr(b.getStringAttr("WrongTop"));
  if(mode==1)c->removeAttr("rawAnnotations");
  if(mode==2)top(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
  if(mode==3){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryWriteArbiter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
  if(mode==4){for(auto i:m.getOps<InstanceOp>())if(i.getName()=="memory_buffer"){i.setNameAttr(b.getStringAttr("wrong"));break;}}
  if(mode==5){for(auto conn:m.getOps<ConnectOp>()){conn.erase();break;}}
  if(mode==6){auto key=top(c,"GGFASEDTokenEngine")->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");NamedAttrList k(key),ws(key.getAs<DictionaryAttr>("axi4Widths"));ws.set("idBits",b.getI64IntegerAttr(5));k.set("axi4Widths",ws.getDictionary(&ctx));top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));}
  if(mode==7){for(auto conn:m.getOps<StrictConnectOp>()){conn.erase();break;}}
  if(mode==8){b.setInsertionPointToStart(top(c,"UnusedFixture").getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used_top");}
  auto before=dump(root->getOperation());std::string error;require(failed(goldengate::addHostMemoryWriteArbiter(c,error))&&!error.empty(),"contract rejection");require(dump(root->getOperation())==before,"rejection mutated circuit");
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    auto before=dump(top(c,"GGFASEDAddressTranslationWrapper"));std::string error;
    require(succeeded(goldengate::addHostMemoryWriteArbiter(c,error)),error);require(succeeded(verify(*root)),"IR verifier");
    require(before==dump(top(c,"GGFASEDAddressTranslationWrapper")),"inner module changed");
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==5,"annotation count");
    auto target=[&](unsigned i){return cast<DictionaryAttr>(as[i]).getAs<StringAttr>("target").getValue();};
    require(target(0)=="~GGHostMemoryWriteWrapper|GGHostMemoryWriteWrapper>diagnostic","copied port identity");
    require(target(1)=="~GGHostMemoryWriteWrapper|GGFASEDAddressTranslationWrapper>loadmem_mem_aw_bits_addr","consumed LoadMem identity");
    require(target(2)=="~GGHostMemoryWriteWrapper|GGFASEDAddressTranslationWrapper>fased_host_mem.aw.bits.id","consumed FASED identity");
    require(target(3)=="~GGHostMemoryWriteWrapper|GGHostMemoryWriteWrapper>fased_host_mem.r.bits.id","remaining channel identity");
    require(target(4)=="~GGHostMemoryWriteWrapper|GGFASEDAddressTranslationWrapper>fased_host_mem","aggregate identity");
    exhaustiveQueue(c);trace(c);for(unsigned i=0;i<9;++i)rejected(ctx,i);
    llvm::outs()<<"Nine atomic rejections and retained target identities pass\n";
    if(argc>=3) {
      auto handoff=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(handoff),"cannot parse real buffered boundary");
      auto real=*handoff->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::addHostMemoryWriteArbiter(real,error)),error);
      require(succeeded(verify(*handoff)),"real boundary IR verifier");
      for(auto n:{"GGHostMemoryWriteSourceQueue2","GGHostMemoryWriteArbiter"})
        require(dump(top(real,n))==dump(top(c,n)),"real and tested helper differ");
      llvm::outs()<<"Real Rocket buffered boundary: valid wrapper, identical tested helpers, "
        <<real->getAttrOfType<ArrayAttr>("rawAnnotations").size()<<" annotations\n";
    }
    if(argc>=2) {
      // Only emit the executable helper fragment; the synthetic inner fixture
      // deliberately models a pre-existing boundary, not a full simulator.
      std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"cannot write helper");
      out<<"module { firrtl.circuit \"GGHostMemoryWriteArbiter\" {\n";
      top(c,"GGHostMemoryWriteSourceQueue2")->print(out);out<<"\n";top(c,"GGHostMemoryWriteArbiter")->print(out);out<<"\n} }\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
