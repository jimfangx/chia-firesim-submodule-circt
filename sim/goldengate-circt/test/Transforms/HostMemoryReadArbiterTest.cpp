// See LICENSE for license details.
#include "goldengate/HostMemoryReadArbiter.h"
#include "goldengate/HostMemoryWriteResponses.h"
#include "goldengate/HostMemoryWriteArbiter.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <vector>
#include <random>
#include "llvm/ADT/DenseMap.h"
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
// Evaluate the generated helper's SSA and synchronous host-clock updates.
struct Interpreter {
  FModuleOp module;std::map<std::string,Value> drivers;
  std::map<std::string,APInt> inputs,memo;llvm::DenseMap<Value,APInt> state,next;
  std::string key(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    if(auto r=v.getDefiningOp<RegResetOp>())return r.getName().str();
    auto a=cast<BlockArgument>(v);return module.getPortName(a.getArgNumber()).str();
  }
  Interpreter(FModuleOp m):module(m) {
    for(auto c:m.getOps<StrictConnectOp>())require(drivers.emplace(key(c.getDest()),c.getSrc()).second,"multiple drivers");
    for(auto r:m.getOps<RegResetOp>())state[r.getResult()]=eval(r.getResetValue());
  }
  APInt eval(Value v) {
    unsigned w=*cast<UIntType>(v.getType()).getWidth();APInt n(w,0);auto *op=v.getDefiningOp();
    auto x=[&](unsigned i){return eval(op->getOperand(i));};
    if(isa_and_nonnull<RegResetOp>(op))n=state.lookup(v);
    else if(auto k=dyn_cast_or_null<ConstantOp>(op))n=k.getValue();
    else if(isa_and_nonnull<AndPrimOp>(op))n=x(0)&x(1);
    else if(isa_and_nonnull<OrPrimOp>(op))n=x(0)|x(1);
    else if(isa_and_nonnull<NotPrimOp>(op))n=~x(0);
    else if(isa_and_nonnull<NEQPrimOp>(op))n=APInt(1,x(0)!=x(1));
    else if(isa_and_nonnull<MuxPrimOp>(op))n=x(x(0).isZero()?2:1);
    else if(isa_and_nonnull<CatPrimOp>(op)){auto a=x(0),b=x(1);n=(a.zext(w)<<b.getBitWidth())|b.zext(w);}
    else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op))n=x(0).lshr(bits.getLo()).trunc(w);
    else n=at(key(v),w);
    return n.zextOrTrunc(w);
  }
  APInt at(std::string n,unsigned w) {
    if(memo.count(n))return memo.at(n);
    APInt v=drivers.count(n)?eval(drivers.at(n)):inputs.at(n);
    v=v.zextOrTrunc(w);memo.insert_or_assign(n,v);return v;
  }
  void input(std::string n,unsigned w,uint64_t v){inputs.insert_or_assign(n,APInt(w,v));}
  void prepare() {
    next.clear();
    for(auto a:module.getOps<AssertOp>())require(eval(a->getOperand(2)).isZero()||!eval(a->getOperand(1)).isZero(),"AR winner assertion");
    for(auto r:module.getOps<RegResetOp>())next[r.getResult()]=eval(r.getResetSignal()).isZero()?eval(drivers.at(key(r.getResult()))):eval(r.getResetValue());
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto addr=token("id: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>");
  auto mem="!firrtl.bundle<aw: "+addr+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<4>, resp: uint<2>")+", ar: "+addr+", r flip: "+token("id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+">";
  std::string ports="in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_mem: "+mem;
  for(auto [n,w]:std::vector<std::pair<std::string,unsigned>>{{"aw_ready",1},{"aw_valid",1},{"aw_bits_addr",34},{"aw_bits_len",8},{"w_ready",1},{"w_valid",1},{"w_bits_data",64},{"w_bits_last",1}})
    ports+=", "+std::string(n.find("ready")!=std::string::npos?"in":"out")+" %loadmem_mem_"+n+": !firrtl.uint<"+std::to_string(w)+">";
  ports+=", out %loadmem_mem_b_ready: !firrtl.uint<1>, in %loadmem_mem_b_valid: !firrtl.uint<1>";
  ports+=", in %loadmem_mem_ar_ready: !firrtl.uint<1>, out %loadmem_mem_ar_valid: !firrtl.uint<1>, out %loadmem_mem_ar_bits_addr: !firrtl.uint<34>";
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
  circuit->setAttr("rawAnnotations",b.getArrayAttr(ann));
  std::string error;require(succeeded(goldengate::addHostMemoryWriteArbiter(circuit,error)),error);
  ann.clear();
  for(auto n:{"diagnostic","loadmem_mem_b_valid","fased_host_mem.b.bits.id","fased_host_mem.r.bits.id","fased_host_mem","host_mem_write.aw.bits.id","host_mem_write"})
    ann.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~GGHostMemoryWriteWrapper|GGHostMemoryWriteWrapper>"+std::string(n)))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(ann));
  require(succeeded(goldengate::routeHostMemoryWriteResponses(circuit,error)),error);
  ann.clear();
  for(auto n:{"diagnostic","loadmem_mem_ar_bits_addr","fased_host_mem.ar.bits.id","fased_host_mem.r.bits.id","fased_host_mem","host_mem_write.aw.bits.id","host_mem_write","host_mem_write.b.bits.id"})
    ann.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~GGHostMemoryWriteResponseWrapper|GGHostMemoryWriteResponseWrapper>"+std::string(n)))}));
  circuit->setAttr("rawAnnotations",b.getArrayAttr(ann));return root;
}

Value reg(FModuleOp m,StringRef name){for(auto r:m.getOps<RegResetOp>())if(r.getName()==name)return r.getResult();throw std::runtime_error("missing register");}
struct State {unsigned idle=1,s0=0,s1=0,mask=3;};
// These equations are the recorded AXI4Xbar AR cone: rightOR/filter,
// allowed/readys, Mux1H, and the four always @(posedge clock) updates.
unsigned step(Interpreter &sim,State &st,bool reset,bool v0,bool v1,bool ready,std::mt19937_64 &rng) {
  sim.memo.clear();sim.input("reset",1,reset);sim.input("load.valid",1,v0);sim.input("fased.valid",1,v1);sim.input("out.ready",1,ready);
  std::map<std::string,unsigned> fields{{"id",4},{"qos",4},{"prot",3},{"cache",4},{"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",34}};
  uint64_t addr=rng()&((1ULL<<34)-1);sim.input("load.bits.addr",34,addr);
  unsigned vs=(v1<<1)|v0,filter=((vs&(~st.mask&3))<<2)|vs;
  unsigned unready=((filter|(filter>>1))>>1)|(st.mask<<2),readys=~((unready>>2)&unready)&3,winner=readys&vs;
  unsigned s0=st.idle?(winner&1):st.s0,s1=st.idle?((winner>>1)&1):st.s1;
  bool valid=st.idle?bool(vs):bool((st.s0&&v0)||(st.s1&&v1));unsigned checks=0;
  for(auto [n,w]:fields) {
    uint64_t f=rng()&((1ULL<<w)-1);sim.input("fased.bits."+n,w,f);
    uint64_t l=n=="addr"?addr:n=="id"?16:n=="size"?3:n=="burst"?1:0;
    unsigned ow=n=="id"?5:w;require(sim.at("out.bits."+n,ow).getZExtValue()==((s0?l:0)|(s1?f:0)),"AR payload mismatch");++checks;
  }
  require(sim.at("out.valid",1).getBoolValue()==valid,"AR output valid");
  require(sim.at("load.ready",1).getBoolValue()==(ready&&(st.idle?(readys&1):st.s0)),"AR LoadMem readiness");
  require(sim.at("fased.ready",1).getBoolValue()==(ready&&(st.idle?(readys&2):st.s1)),"AR FASED readiness");checks+=3;
  State next{reset?1U:unsigned((valid&&ready)||(vs?false:bool(st.idle))),reset?0U:s0,reset?0U:s1,reset?3U:st.idle&&vs?(winner|((winner<<1)&3)):st.mask};
  sim.prepare();
  for(auto [n,w,x]:std::vector<std::tuple<std::string,unsigned,unsigned>>{{"idle",1,next.idle},{"state_0",1,next.s0},{"state_1",1,next.s1},{"readys_mask",2,next.mask}}) {
    require(sim.next.lookup(reg(sim.module,n)).getZExtValue()==x,"AR next register mismatch");++checks;
  }
  sim.state=sim.next;st=next;return checks;
}
void compare(CircuitOp c) {
  auto m=top(c,"GGHostMemoryReadArbiter");Interpreter sim(m);std::mt19937_64 rng(244);unsigned checks=0;
  for(unsigned code=0;code<512;++code) {
    State st{code&1,(code>>1)&1,(code>>2)&1,(code>>3)&3};
    sim.state[reg(m,"idle")]=APInt(1,st.idle);sim.state[reg(m,"state_0")]=APInt(1,st.s0);sim.state[reg(m,"state_1")]=APInt(1,st.s1);sim.state[reg(m,"readys_mask")]=APInt(2,st.mask);
    checks+=step(sim,st,(code>>8)&1,(code>>5)&1,(code>>6)&1,(code>>7)&1,rng);
  }
  State st;unsigned traceChecks=0,stalls=0,contention=0;
  // Exercise initial and in-flight resets, extended stalls, repeated contention,
  // and disappearing requests (legal-state-independent combinational probes).
  for(unsigned cycle=0;cycle<12001;++cycle) {
    bool reset=cycle==0||cycle%997==0,v0=cycle%5<3,v1=cycle%7<5,ready=cycle%19>=8;
    traceChecks+=step(sim,st,reset,v0,v1,ready,rng);stalls+=!ready;contention+=v0&&v1;
  }
  require(std::distance(m.getOps<RegResetOp>().begin(),m.getOps<RegResetOp>().end())==4,"four AR host registers");
  llvm::outs()<<"AXI4Xbar AR: 512 exhaustive state/input/reset cases, "<<checks<<" checks; 12001-cycle trace, "<<traceChecks<<" checks, "<<stalls<<" stalled cycles, "<<contention<<" contention cycles\n";
}
void bindings(CircuitOp c) {
  auto m=top(c,"GGHostMemoryReadWrapper");
  auto path=[&](Value v){
    std::string suffix;while(auto f=v.getDefiningOp<SubfieldOp>()){suffix="."+f.getFieldName().str()+suffix;v=f.getInput();}
    if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str()+suffix;
    auto i=v.getDefiningOp<InstanceOp>();require(bool(i),"unknown wrapper root");
    return i.getName().str()+"."+i.getPortName(cast<OpResult>(v).getResultNumber()).str()+suffix;
  };
  std::map<std::string,std::string> got;
  for(auto conn:m.getOps<ConnectOp>())require(got.emplace(path(conn.getDest()),path(conn.getSrc())).second,"duplicate aggregate connection");
  for(auto conn:m.getOps<StrictConnectOp>())require(got.emplace(path(conn.getDest()),path(conn.getSrc())).second,"duplicate scalar connection");
  const std::map<std::string,std::string> expected{
    {"host_mem.aw","sim.host_mem_write.aw"},{"host_mem.w","sim.host_mem_write.w"},{"sim.host_mem_write.b","host_mem.b"},
    {"read_arbiter.clock","hostClock"},{"read_arbiter.reset","hostReset"},
    {"sim.loadmem_mem_ar_ready","read_arbiter.load.ready"},{"read_arbiter.load.valid","sim.loadmem_mem_ar_valid"},
    {"read_arbiter.load.bits.addr","sim.loadmem_mem_ar_bits_addr"},{"read_arbiter.fased","sim.fased_host_mem.ar"},
    {"host_mem.ar","read_arbiter.out"},{"sim.fased_host_mem.r","fased_host_mem.r"}};
  for(auto [d,s]:expected)require(got.at(d)==s,"wrapper binding mismatch");
  llvm::outs()<<"Eleven host/LoadMem/FASED boundary bindings pass\n";
}
void rejected(MLIRContext &ctx,unsigned mode) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();auto m=top(c,"GGHostMemoryWriteResponseWrapper");OpBuilder b(&ctx);
  if(mode==0)c.setNameAttr(b.getStringAttr("WrongTop"));
  if(mode==1)c->removeAttr("rawAnnotations");
  if(mode==2)top(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
  if(mode==3){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryReadArbiter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
  if(mode==4){for(auto i:m.getOps<InstanceOp>())if(i.getName()=="write_responses"){i.setNameAttr(b.getStringAttr("wrong"));break;}}
  if(mode==5||mode==9){for(auto conn:m.getOps<ConnectOp>()){auto f=conn.getDest().getDefiningOp<SubfieldOp>();if(f&&f.getFieldName()=="ar"){if(mode==5)conn.erase();else{b.setInsertionPoint(conn);b.create<ConnectOp>(conn.getLoc(),conn.getDest(),conn.getSrc());}break;}}}
  if(mode==6){auto key=top(c,"GGFASEDTokenEngine")->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");NamedAttrList k(key),ws(key.getAs<DictionaryAttr>("axi4Widths"));ws.set("idBits",b.getI64IntegerAttr(5));k.set("axi4Widths",ws.getDictionary(&ctx));top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));}
  if(mode==7){b.setInsertionPointToStart(top(c,"UnusedFixture").getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used_top");}
  if(mode==8){auto names=m.getPortNamesAttr();SmallVector<Attribute> out(names.begin(),names.end());for(auto &n:out)if(cast<StringAttr>(n).getValue()=="loadmem_mem_ar_valid")n=b.getStringAttr("wrong");m.setPortNamesAttr(b.getArrayAttr(out));}
  if(mode==10){for(auto conn:m.getOps<ConnectOp>()){auto a=dyn_cast<BlockArgument>(conn.getSrc());if(a&&m.getPortName(a.getArgNumber())=="hostClock"){conn.erase();break;}}}
  auto before=dump(root->getOperation());std::string error;
  require(failed(goldengate::addHostMemoryReadArbiter(c,error))&&!error.empty(),"contract rejection");
  require(dump(root->getOperation())==before,"rejection mutated circuit");
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    std::map<std::string,std::string> before;for(auto m:c.getOps<FModuleOp>())before[m.getName().str()]=dump(m);
    std::string error;require(succeeded(goldengate::addHostMemoryReadArbiter(c,error)),error);require(succeeded(verify(*root)),"IR verifier");
    for(auto [n,s]:before)require(dump(top(c,n))==s,"pre-existing module changed");
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==8,"annotation count");
    std::vector<std::string> refs{"diagnostic","loadmem_mem_ar_bits_addr","fased_host_mem.ar.bits.id","fased_host_mem.r.bits.id","fased_host_mem","host_mem.aw.bits.id","host_mem_write","host_mem.b.bits.id"};
    for(unsigned i=0;i<8;++i) {
      bool moved=i==0||i==3||i==5||i==7;
      auto target=cast<DictionaryAttr>(as[i]).getAs<StringAttr>("target").getValue();
      require(target=="~GGHostMemoryReadWrapper|"+std::string(moved?"GGHostMemoryReadWrapper":"GGHostMemoryWriteResponseWrapper")+">"+refs[i],"target identity mismatch");
    }
    compare(c);bindings(c);for(unsigned i=0;i<11;++i)rejected(ctx,i);
    llvm::outs()<<"Eleven atomic rejections, eight target identities and preserved inner bodies pass\n";
    if(argc>=3) {
      auto realRoot=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(realRoot),"cannot parse real AW/W/B boundary");auto real=*realRoot->getOps<CircuitOp>().begin();
      auto prior=dump(top(real,"GGHostMemoryWriteResponseWrapper"));
      require(succeeded(goldengate::addHostMemoryReadArbiter(real,error)),error);require(succeeded(verify(*realRoot)),"real boundary verifier");bindings(real);
      require(dump(top(real,"GGHostMemoryWriteResponseWrapper"))==prior,"real inner body changed");
      require(dump(top(real,"GGHostMemoryReadArbiter"))==dump(top(c,"GGHostMemoryReadArbiter")),"real and tested helper differ");
      llvm::outs()<<"Real Rocket boundary: "<<top(real,"GGHostMemoryReadWrapper").getNumPorts()<<" ports, "<<real->getAttrOfType<ArrayAttr>("rawAnnotations").size()<<" annotations\n";
    }
    if(argc>=2) {
      std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"cannot write helper");out<<"module { firrtl.circuit \"GGHostMemoryReadArbiter\" {\n";
      top(c,"GGHostMemoryReadArbiter")->print(out);out<<"\n} }\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
