// See LICENSE for license details.
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
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
// Evaluate the actual generated SSA. Reference values below come from the
// immutable AXI4Xbar response cone, independently of the pass implementation.
struct Interpreter {
  FModuleOp module;std::map<std::string,Value> drivers;
  std::map<std::string,APInt> inputs,memo;
  std::string key(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    auto a=cast<BlockArgument>(v);return module.getPortName(a.getArgNumber()).str();
  }
  Interpreter(FModuleOp m):module(m) {
    for(auto c:m.getOps<StrictConnectOp>())require(drivers.emplace(key(c.getDest()),c.getSrc()).second,"multiple drivers");
  }
  APInt eval(Value v) {
    unsigned w=*cast<UIntType>(v.getType()).getWidth();APInt n(w,0);auto *op=v.getDefiningOp();
    auto x=[&](unsigned i){return eval(op->getOperand(i));};
    if(auto k=dyn_cast_or_null<ConstantOp>(op))n=k.getValue();
    else if(isa_and_nonnull<AndPrimOp>(op))n=x(0)&x(1);
    else if(isa_and_nonnull<OrPrimOp>(op))n=x(0)|x(1);
    else if(isa_and_nonnull<NotPrimOp>(op))n=~x(0);
    else if(isa_and_nonnull<EQPrimOp>(op))n=APInt(1,x(0)==x(1));
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
};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto token=[](StringRef fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs.str()+">>";};
  auto addr=token("id: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>");
  auto mem="!firrtl.bundle<aw: "+addr+", w: "+token("strb: uint<8>, last: uint<1>, data: uint<64>")+", b flip: "+token("id: uint<4>, resp: uint<2>")+", ar: "+addr+", r flip: "+token("id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+">";
  std::string ports="in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_mem: "+mem;
  for(auto [n,w]:std::vector<std::pair<std::string,unsigned>>{{"aw_ready",1},{"aw_valid",1},{"aw_bits_addr",34},{"aw_bits_len",8},{"w_ready",1},{"w_valid",1},{"w_bits_data",64},{"w_bits_last",1}})
    ports+=", "+std::string(n.find("ready")!=std::string::npos?"in":"out")+" %loadmem_mem_"+n+": !firrtl.uint<"+std::to_string(w)+">";
  ports+=", out %loadmem_mem_b_ready: !firrtl.uint<1>, in %loadmem_mem_b_valid: !firrtl.uint<1>";
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
  circuit->setAttr("rawAnnotations",b.getArrayAttr(ann));return root;
}

void compare(CircuitOp c) {
  auto m=top(c,"GGHostMemoryWriteResponseRouter");Interpreter sim(m);unsigned cases=0,checks=0,blocked=0;
  for(unsigned id=0;id<32;++id)for(unsigned resp=0;resp<4;++resp)
    for(unsigned v=0;v<2;++v)for(unsigned lr=0;lr<2;++lr)for(unsigned fr=0;fr<2;++fr) {
      sim.input("host.bits.id",5,id);sim.input("host.bits.resp",2,resp);sim.input("host.valid",1,v);
      sim.input("load.ready",1,lr);sim.input("fased.ready",1,fr);sim.memo.clear();
      std::map<std::string,std::pair<unsigned,unsigned>> expected{
        {"host.ready",{1,((id==16)&&lr)||((id<16)&&fr)}},
        {"load.valid",{1,v&&(id==16)}},{"fased.valid",{1,v&&(id<16)}},
        {"fased.bits.id",{4,id%16}},{"fased.bits.resp",{2,resp}}};
      for(auto [n,p]:expected){require(sim.at(n,p.first).getZExtValue()==p.second,"AXI4Xbar B cone mismatch");++checks;}
      ++cases;blocked+=id>16;
    }
  require(m.getOps<RegResetOp>().empty()&&m.getOps<InstanceOp>().empty(),"B router must be combinational");
  llvm::outs()<<"AXI4Xbar B cone: "<<cases<<" exhaustive cases, "<<checks<<" leaf checks, "<<blocked<<" unmatched-ID cases\n";
}
// Wrapper connections include aggregate flips. Check exact root+field pairs,
// including the reversed B/R connects, before relying on helper behavior.
void bindings(CircuitOp c) {
  auto m=top(c,"GGHostMemoryWriteResponseWrapper");
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
    {"host_mem_write.aw","sim.host_mem_write.aw"},{"host_mem_write.w","sim.host_mem_write.w"},
    {"write_responses.host","host_mem_write.b"},{"write_responses.load.ready","sim.loadmem_mem_b_ready"},
    {"sim.loadmem_mem_b_valid","write_responses.load.valid"},{"sim.fased_host_mem.b","write_responses.fased"},
    {"fased_host_mem.ar","sim.fased_host_mem.ar"},{"sim.fased_host_mem.r","fased_host_mem.r"}};
  for(auto [d,s]:expected)require(got.at(d)==s,"wrapper binding mismatch");
  llvm::outs()<<"Eight host/LoadMem/FASED boundary bindings pass\n";
}
void rejected(MLIRContext &ctx,unsigned mode) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();auto m=top(c,"GGHostMemoryWriteWrapper");OpBuilder b(&ctx);
  if(mode==0)c.setNameAttr(b.getStringAttr("WrongTop"));
  if(mode==1)c->removeAttr("rawAnnotations");
  if(mode==2)top(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
  if(mode==3){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryWriteResponseRouter"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
  if(mode==4){for(auto i:m.getOps<InstanceOp>())if(i.getName()=="write_arbiter"){i.setNameAttr(b.getStringAttr("wrong"));break;}}
  if(mode==5){for(auto conn:m.getOps<ConnectOp>()){auto f=conn.getSrc().getDefiningOp<SubfieldOp>();if(f&&f.getFieldName()=="b"){conn.erase();break;}}}
  if(mode==6){auto key=top(c,"GGFASEDTokenEngine")->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor");NamedAttrList k(key),ws(key.getAs<DictionaryAttr>("axi4Widths"));ws.set("idBits",b.getI64IntegerAttr(5));k.set("axi4Widths",ws.getDictionary(&ctx));top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",k.getDictionary(&ctx));}
  if(mode==7){b.setInsertionPointToStart(top(c,"UnusedFixture").getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used_top");}
  if(mode==8){auto names=m.getPortNamesAttr();SmallVector<Attribute> out(names.begin(),names.end());for(auto &n:out)if(cast<StringAttr>(n).getValue()=="loadmem_mem_b_valid")n=b.getStringAttr("wrong");m.setPortNamesAttr(b.getArrayAttr(out));}
  if(mode==9){for(auto conn:m.getOps<ConnectOp>()){auto f=conn.getSrc().getDefiningOp<SubfieldOp>();if(f&&f.getFieldName()=="b"){b.setInsertionPoint(conn);b.create<ConnectOp>(conn.getLoc(),conn.getDest(),conn.getSrc());break;}}}
  auto before=dump(root->getOperation());std::string error;
  require(failed(goldengate::routeHostMemoryWriteResponses(c,error))&&!error.empty(),"contract rejection");
  require(dump(root->getOperation())==before,"rejection mutated circuit");
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
    std::map<std::string,std::string> before;for(auto m:c.getOps<FModuleOp>())before[m.getName().str()]=dump(m);
    std::string error;require(succeeded(goldengate::routeHostMemoryWriteResponses(c,error)),error);
    require(succeeded(verify(*root)),"IR verifier");
    for(auto [n,s]:before)require(dump(top(c,n))==s,"pre-existing module changed");
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==7,"annotation count");
    auto target=[&](unsigned i){return cast<DictionaryAttr>(as[i]).getAs<StringAttr>("target").getValue();};
    std::string prefix="~GGHostMemoryWriteResponseWrapper|";
    for(unsigned i=0;i<7;++i) {
      bool copied=i==0||i==3||i==5;
      std::string n=std::vector<std::string>{"diagnostic","loadmem_mem_b_valid","fased_host_mem.b.bits.id","fased_host_mem.r.bits.id","fased_host_mem","host_mem_write.aw.bits.id","host_mem_write"}[i];
      require(target(i)==prefix+(copied?"GGHostMemoryWriteResponseWrapper":"GGHostMemoryWriteWrapper")+">"+n,"target identity mismatch");
    }
    compare(c);bindings(c);for(unsigned i=0;i<10;++i)rejected(ctx,i);
    llvm::outs()<<"Ten atomic rejections, seven target identities and preserved inner bodies pass\n";
    if(argc>=3) {
      auto realRoot=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(realRoot),"cannot parse real AW/W boundary");
      auto real=*realRoot->getOps<CircuitOp>().begin();
      // Production comparison checks every old module as a single text scan.
      // Printing each isolated suboperation rebuilds assembly state over the
      // complete 46 MB circuit; only snapshot this pass's boundary here.
      auto prior=dump(top(real,"GGHostMemoryWriteWrapper"));
      require(succeeded(goldengate::routeHostMemoryWriteResponses(real,error)),error);
      require(succeeded(verify(*realRoot)),"real boundary verifier");bindings(real);
      require(dump(top(real,"GGHostMemoryWriteWrapper"))==prior,"real inner body changed");
      require(dump(top(real,"GGHostMemoryWriteResponseRouter"))==dump(top(c,"GGHostMemoryWriteResponseRouter")),"real and tested helper differ");
      llvm::outs()<<"Real Rocket boundary: preserved AW/W wrapper, "<<top(real,"GGHostMemoryWriteResponseWrapper").getNumPorts()<<" ports, "<<real->getAttrOfType<ArrayAttr>("rawAnnotations").size()<<" annotations\n";
    }
    if(argc>=2) {
      std::error_code ec;llvm::raw_fd_ostream out(argv[1],ec);require(!ec,"cannot write helper");
      out<<"module { firrtl.circuit \"GGHostMemoryWriteResponseRouter\" {\n";
      top(c,"GGHostMemoryWriteResponseRouter")->print(out);out<<"\n} }\n";
    }
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
