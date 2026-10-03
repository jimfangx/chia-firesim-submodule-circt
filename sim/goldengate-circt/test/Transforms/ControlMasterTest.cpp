// See LICENSE for license details.
#include "goldengate/SimulationMasterControl.h"
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
void require(bool v,llvm::StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(ModuleOp m){std::string s;llvm::raw_string_ostream os(s);m.print(os);return s;}
FModuleOp top(CircuitOp c,llvm::StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
struct Pin {std::string name,channel,leaf;unsigned width;bool input;};
std::vector<Pin> pins(){
  std::vector<Pin> p;
  for(auto ch:{"aw","w","b","r"}) {
    bool request=StringRef(ch)=="aw"||StringRef(ch)=="w";
    std::string prefix=request?"ctrl_write_route_"+std::string(ch):"ctrl_"+std::string(StringRef(ch)=="r"?"read":"write")+"_arb_out";
    p.push_back({prefix+"_ready",ch,"ready",1,!request});
    p.push_back({prefix+"_valid",ch,"valid",1,request});
    const std::pair<const char*,unsigned> aw[]{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},{"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
    const std::pair<const char*,unsigned> w[]{{"data",32},{"last",1},{"id",12},{"strb",4},{"user",1}};
    const std::pair<const char*,unsigned> b[]{{"resp",2},{"id",12},{"user",1}};
    const std::pair<const char*,unsigned> r[]{{"resp",2},{"data",32},{"last",1},{"id",12},{"user",1}};
    auto fs=StringRef(ch)=="aw"?ArrayRef(aw):StringRef(ch)=="w"?ArrayRef(w):StringRef(ch)=="b"?ArrayRef(b):ArrayRef(r);
    for(auto [n,width]:fs) {
      auto name=request?"ctrl_write_dispatch_master_"+std::string(ch)+"_bits_"+n:prefix+"_bits_"+n;
      if(StringRef(ch)=="aw"&&StringRef(n)=="addr")name="ctrl_decode_aw_addr";
      if(StringRef(ch)=="w"&&StringRef(n)=="last")name="ctrl_write_route_w_last";
      p.push_back({name,ch,"bits."+std::string(n),width,request});
    }
  }
  return p;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0){
  std::string s="module { firrtl.circuit \"GGFASEDBridgeBoundWrapper\" { firrtl.module @GGFASEDBridgeBoundWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %diagnostic: !firrtl.uint<4>";
  for(auto p:pins()) {
    if(bad==1&&p.name=="ctrl_write_route_w_last")continue;
    if(bad==2&&p.name=="ctrl_write_dispatch_master_aw_bits_id")p.width=11;
    if(bad==3&&p.name=="ctrl_read_arb_out_ready")p.input=false;
    s+=", "+std::string(p.input?"in":"out")+" %"+p.name+": !firrtl.uint<"+std::to_string(p.width)+">";
  }
  s+=", in %ctrl_read_dispatch_master_ar: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>>";
  if(bad==4)s+=", in %ctrl: !firrtl.uint<1>";
  s+=") {} } }";
  if(bad==9)s.replace(s.find("user: uint<1>>>"),std::string("user: uint<1>>>").size(),"user: uint<2>>>");
  if(bad==10)s.replace(s.find("in %ctrl_read_dispatch_master_ar"),2,"out");
  auto root=parseSourceString<ModuleOp>(s,&ctx);require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);SmallVector<Attribute> annos;
  for(auto n:{"diagnostic","ctrl_write_route_w_last","ctrl_read_dispatch_master_ar.bits.id"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~GGFASEDBridgeBoundWrapper|GGFASEDBridgeBoundWrapper>"+std::string(n)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if(bad==5)c->removeAttr("rawAnnotations");
  auto m=top(c,"GGFASEDBridgeBoundWrapper");
  if(bad==6){b.setInsertionPointToStart(m.getBodyBlock());b.create<InstanceOp>(c.getLoc(),m,"used");}
  if(bad==7)c.setName("Wrong");
  if(bad==8){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlMasterWrapper"),m.getConventionAttr(),ArrayRef<PortInfo>{});}
  return root;
}
}
int main(){
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
    auto inner=top(c,"GGFASEDBridgeBoundWrapper");std::string before;llvm::raw_string_ostream out(before);inner.print(out);
    require(succeeded(goldengate::bindControlMaster(c,error)),error);
    require(succeeded(verify(*root)),"invalid bound IR");
    auto m=top(c,"GGControlMasterWrapper");require(m.getNumPorts()==4,"master scalar boundaries remain exposed");
    InstanceOp sim=*m.getOps<InstanceOp>().begin();
    std::function<std::string(Value)> key=[&](Value v)->std::string{
      if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
      if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
      for(unsigned i=0;i<sim.getNumResults();++i)if(v==sim.getResult(i))return "sim_"+inner.getPortName(i).str();
      throw std::runtime_error("unknown wire");
    };
    std::map<std::string,std::string> actual,expected;
    for(auto x:m.getOps<StrictConnectOp>())require(actual.emplace(key(x.getDest()),key(x.getSrc())).second,"duplicate driver");
    for(auto p:pins()) {
      auto outside="ctrl."+p.channel+"."+p.leaf,inside="sim_"+p.name;
      expected[p.input?inside:outside]=p.input?outside:inside;
    }
    require(actual==expected&&actual.size()==32,"master payload/handshake direction differs");
    bool ar=false;unsigned copied=0;
    for(auto x:m.getOps<ConnectOp>()) {
      if(key(x.getDest())=="sim_ctrl_read_dispatch_master_ar") {require(key(x.getSrc())=="ctrl.ar","AR request/ready direction differs");ar=true;}
      else {++copied;auto d=key(x.getDest()),s=key(x.getSrc());require(d=="sim_"+s||s=="sim_"+d,"copied clock/diagnostic wiring differs");}
    }
    require(ar&&copied==3,"master AR or copied boundary missing");
    std::string after;llvm::raw_string_ostream os(after);inner.print(os);require(before==after,"inner module changed");
    auto as=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(as.size()==3,"annotations lost");
    for(auto [i,a]:llvm::enumerate(as)) {
      auto target=cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();
      require(target.starts_with("~GGControlMasterWrapper|"),"circuit target lost");
      require(target.contains(i==0?"|GGControlMasterWrapper>":"|GGFASEDBridgeBoundWrapper>"),"internalized target escaped");
    }
    auto good=dump(*root);require(failed(goldengate::bindControlMaster(c,error))&&dump(*root)==good,"repeat mutated IR");
    for(unsigned bad=1;bad<=10;++bad){auto r=fixture(ctx,bad);auto c=*r->getOps<CircuitOp>().begin();auto original=dump(*r);require(failed(goldengate::bindControlMaster(c,error)),"invalid boundary accepted");require(original==dump(*r),"rejection mutated IR");}
    llvm::outs()<<"Control master: 32 scalar and aggregate AR connections, copied clocks, target transfer and 11 atomic rejections passed\n";
    return 0;
  } catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
