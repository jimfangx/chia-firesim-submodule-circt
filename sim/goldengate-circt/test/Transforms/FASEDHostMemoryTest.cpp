// See LICENSE for license details.
#include "goldengate/FASEDHostMemory.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os);return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0){
  std::string address="user: uint<1>, id: uint<4>, region: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>";
  auto token=[](std::string fs){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<"+fs+">>";};
  std::string s="module { firrtl.circuit \"GGControlMasterWrapper\" { firrtl.module @GGControlMasterWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>, out %fased_host_requests: !firrtl.bundle<aw: "+token(address)+", w: "+token("user: uint<1>, strb: uint<8>, id: uint<4>, last: uint<1>, data: uint<64>")+", ar: "+token(address)+">, in %fased_host_read_response: !firrtl."+token("user: uint<1>, id: uint<4>, last: uint<1>, data: uint<64>, resp: uint<2>")+", in %fased_host_write_response: !firrtl."+token("user: uint<1>, id: uint<4>, resp: uint<2>");
  if(bad==3)s+=", out %fased_host_mem: !firrtl.uint<1>";
  s+=") {} firrtl.module @GGFASEDTokenEngine() {} } }";
  if(bad==1)s.replace(s.find("addr: uint<35>"),std::string("addr: uint<35>").size(),"addr: uint<34>");
  if(bad==2)s.replace(s.find("in %fased_host_read_response"),2,"out");
  if(bad==9)s.replace(s.find("strb: uint<8>"),std::string("strb: uint<8>").size(),"strb: uint<7>");
  if(bad==10)s.replace(s.find("ready flip: uint<1>"),std::string("ready flip: uint<1>").size(),"ready: uint<1>");
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> annos;
  for(auto n:{"diagnostic","fased_host_requests.aw.bits.addr","fased_host_read_response.bits.resp","fased_host_write_response.ready","fased_host_requests.w.bits.id","fased_host_requests","fased_host_read_response.bits.user"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGControlMasterWrapper|GGControlMasterWrapper>"+std::string(n))}))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  top(c,"GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({b.getNamedAttr("axi4Widths",b.getDictionaryAttr({b.getNamedAttr("addrBits",b.getI64IntegerAttr(35)),b.getNamedAttr("dataBits",b.getI64IntegerAttr(bad==4?128:64)),b.getNamedAttr("idBits",b.getI64IntegerAttr(4))}))}));
  if(bad==5)c->removeAttr("rawAnnotations");
  if(bad==6){auto m=top(c,"GGControlMasterWrapper");b.setInsertionPointToStart(m.getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used");}
  if(bad==7)c.setName("Wrong");
  if(bad==8){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDHostMemoryWrapper"),top(c,"GGControlMasterWrapper").getConventionAttr(),ArrayRef<PortInfo>{});}
  if(bad==11)top(c,"GGFASEDTokenEngine")->removeAttr("goldengate.bridgeConstructor");
  return root;
}
}
int main(){
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
    auto inner=top(c,"GGControlMasterWrapper");auto before=dump(inner);
    require(succeeded(goldengate::bindFASEDHostMemory(c,error)),error);
    require(succeeded(verify(*root)),"invalid host memory IR");
    auto m=top(c,"GGFASEDHostMemoryWrapper");require(m.getNumPorts()==4,"split bundles still exposed");
    InstanceOp sim=*m.getOps<InstanceOp>().begin();
    std::function<std::string(Value)> key=[&](Value v)->std::string{
      if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
      if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
      for(unsigned i=0;i<sim.getNumResults();++i)if(v==sim.getResult(i))return "sim."+inner.getPortName(i).str();
      return "invalid";
    };
    std::map<std::string,std::string> actual,expected;
    for(auto x:m.getOps<StrictConnectOp>())require(actual.emplace(key(x.getDest()),key(x.getSrc())).second,"duplicate driver");
    for(auto ch:{"aw","w","ar","r","b"}) {
      bool request=StringRef(ch)=="aw"||StringRef(ch)=="w"||StringRef(ch)=="ar";
      auto in=request?"sim.fased_host_requests."+std::string(ch):StringRef(ch)=="r"?"sim.fased_host_read_response":"sim.fased_host_write_response";
      auto out="fased_host_mem."+std::string(ch);
      expected[request?out+".valid":in+".valid"]=request?in+".valid":out+".valid";
      expected[request?in+".ready":out+".ready"]=request?out+".ready":in+".ready";
      std::vector<std::string> fs=StringRef(ch)=="w"?std::vector<std::string>{"strb","last","data"}:StringRef(ch)=="r"?std::vector<std::string>{"id","last","data","resp"}:StringRef(ch)=="b"?std::vector<std::string>{"id","resp"}:std::vector<std::string>{"id","qos","prot","cache","lock","burst","size","len","addr"};
      for(auto f:fs)expected[(request?out:in)+".bits."+f]=(request?in:out)+".bits."+f;
      if(!request)expected[in+".bits.user"]="invalid";
    }
    require(actual==expected&&actual.size()==39,"host memory payload or handshake mismatch");
    require(std::distance(m.getOps<ConnectOp>().begin(),m.getOps<ConnectOp>().end())==3,"copied clocks/diagnostic missing");
    require(before==dump(inner),"inner state changed");
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==7,"annotations lost");
    const char *targets[]{"GGFASEDHostMemoryWrapper>diagnostic","GGFASEDHostMemoryWrapper>fased_host_mem.aw.bits.addr","GGFASEDHostMemoryWrapper>fased_host_mem.r.bits.resp","GGFASEDHostMemoryWrapper>fased_host_mem.b.ready","GGControlMasterWrapper>fased_host_requests.w.bits.id","GGControlMasterWrapper>fased_host_requests","GGControlMasterWrapper>fased_host_read_response.bits.user"};
    for(auto [i,a]:llvm::enumerate(as))require(cast<StringAttr>(cast<DictionaryAttr>(a).getAs<ArrayAttr>("targets")[0]).getValue()=="~GGFASEDHostMemoryWrapper|"+std::string(targets[i]),"target transfer differs");
    auto good=dump(*root);require(failed(goldengate::bindFASEDHostMemory(c,error))&&dump(*root)==good,"repeat mutated IR");
    for(unsigned bad=1;bad<=11;++bad){auto r=fixture(ctx,bad);auto c=*r->getOps<CircuitOp>().begin();auto original=dump(*r);require(failed(goldengate::bindFASEDHostMemory(c,error)),"invalid boundary accepted");require(original==dump(*r),"rejection mutated IR");}
    llvm::outs()<<"FASED host memory: 37 AXI leaves, two invalid users, target transfer and 12 atomic rejections passed\n";return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
