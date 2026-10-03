// See LICENSE for license details.
#include "goldengate/FPGATopShell.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool v,StringRef s){if(!v)throw std::runtime_error(s.str());}
std::string dump(Operation *m){std::string s;llvm::raw_string_ostream os(s);m->print(os,OpPrintingFlags().useLocalScope());return s;}
FModuleOp top(CircuitOp c,StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("missing module");}
void replace(std::string &s,StringRef from,StringRef to){auto i=s.find(from.str());require(i!=std::string::npos,"replacement not found");s.replace(i,from.size(),to.str());}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned mode=0) {
  // Exact recorded U250 ctrl/mem/CPU port contracts; representative private
  // scalar, bundle and clock diagnostics. No SFC implementation logic.
  std::string source=R"mlir(module { firrtl.circuit "GGCPUStreamPlatformWrapper" { firrtl.module @GGCPUStreamPlatformWrapper(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %ep_clock: !firrtl.clock, out %fased_pending_reads: !firrtl.bundle<value: uint<4>, full: uint<1>>, in %ctrl: !firrtl.bundle<aw: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>>, w: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<data: uint<32>, last: uint<1>, id: uint<12>, strb: uint<4>, user: uint<1>>>, b flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<resp: uint<2>, id: uint<12>, user: uint<1>>>, ar: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>>, r flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>>>, out %mem_0: !firrtl.bundle<aw: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>>>, w: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<strb: uint<8>, last: uint<1>, data: uint<64>>>, b flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, resp: uint<2>>>, ar: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<34>>>, r flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, last: uint<1>, data: uint<64>, resp: uint<2>>>>, in %cpu_managed_axi4: !firrtl.bundle<aw: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, addr: uint<64>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>>>, w: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<data: uint<512>, strb: uint<64>, last: uint<1>>>, b flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, resp: uint<2>>>, ar: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, addr: uint<64>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>>>, r flip: bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<id: uint<16>, data: uint<512>, resp: uint<2>, last: uint<1>>>>, out %ctrl_diagnostic: !firrtl.uint<4>) {} } })mlir";
  if(mode==1)replace(source,"out %ctrl_diagnostic","in %ctrl_diagnostic");
  if(mode==2)replace(source,"ctrl_diagnostic","unknown_output");
  if(mode==3)replace(source,"in %hostClock: !firrtl.clock","in %hostClock: !firrtl.uint<1>");
  if(mode==4)replace(source,"in %hostReset: !firrtl.uint<1>","in %hostReset: !firrtl.uint<2>");
  if(mode==5)replace(source,"addr: uint<25>","addr: uint<26>");
  if(mode==6)replace(source,"out %mem_0","in %mem_0");
  if(mode==7)replace(source,"data: uint<512>","data: uint<256>");
  if(mode==8)replace(source,"out %ctrl_diagnostic: !firrtl.uint<4>","out %ctrl_diagnostic: !firrtl.bundle<ready flip: uint<1>>");
  auto root=parseSourceString<ModuleOp>(source,&ctx);require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto m=top(c,"GGCPUStreamPlatformWrapper");
  SmallVector<Attribute> annotations;
  for(auto [cls,n]:{std::pair<StringRef,StringRef>{goldengate::AnnotationClasses::HostClock,"hostClock"},
                   {goldengate::AnnotationClasses::HostReset,"hostReset"},
                   {"test.Target","ctrl.aw.bits.addr"},{"test.Target","mem_0"},
                   {"test.Target","cpu_managed_axi4.r.bits.data"},
                   {"test.Target","ctrl_diagnostic"},{"test.Target","ep_clock"}})
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(cls)),b.getNamedAttr("target",b.getStringAttr("~GGCPUStreamPlatformWrapper|GGCPUStreamPlatformWrapper>"+n.str()))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  SmallVector<PortInfo> simPorts,cpuPorts;
  for(auto p:m.getPorts())if(p.name.getValue()=="cpu_managed_axi4")cpuPorts.push_back(p);else simPorts.push_back(p);
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto child=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGHostMemoryPlatformWrapper"),ConventionAttr::get(&ctx,Convention::Internal),simPorts);
  auto helper=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGCPUStreamPortAdapter"),ConventionAttr::get(&ctx,Convention::Internal),cpuPorts);
  b.setInsertionPointToStart(m.getBodyBlock());auto sim=b.create<InstanceOp>(c.getLoc(),child,"sim"),cpu=b.create<InstanceOp>(c.getLoc(),helper,"cpu_port");
  unsigned k=0;
  for(auto [i,p]:llvm::enumerate(m.getPorts())) {
    Value a=m.getBodyBlock()->getArgument(i),v=p.name.getValue()=="cpu_managed_axi4"?cpu.getResult(0):sim.getResult(k++);
    b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  return root;
}
std::string path(FModuleOp m,Value v) {
  if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
  auto i=v.getDefiningOp<InstanceOp>();require(bool(i),"unknown root");
  return i.getName().str()+"."+i.getPortName(cast<OpResult>(v).getResultNumber()).str();
}
void check(CircuitOp c,unsigned oldCount) {
  auto m=top(c,"FPGATop"),old=top(c,"GGCPUStreamPlatformWrapper");
  require(c.getName()=="FPGATop"&&m.getNumPorts()==5,"public shell must have exactly five ports");
  unsigned instances=0;
  for(auto i:m.getOps<InstanceOp>()) {
    require(i.getModuleName()==old.getName(),"shell must retain original inner module");
    ++instances;
  }
  require(instances==1,"shell must contain exactly one retained child");
  require(m.getOps<RegOp>().empty()&&m.getOps<RegResetOp>().empty()&&m.getOps<MemOp>().empty()&&m.getOps<SubfieldOp>().empty(),"shell adds no state or field adaptation");
  std::map<std::string,std::string> got;
  for(auto conn:m.getOps<ConnectOp>())require(got.emplace(path(m,conn.getDest()),path(m,conn.getSrc())).second,"duplicate shell driver");
  unsigned pi=0;
  for(auto n:{"hostClock","hostReset","ctrl","mem_0","cpu_managed_axi4"}) {
    unsigned i=0;while(old.getPortName(i)!=n)++i;auto p=old.getPorts()[i];auto q=m.getPorts()[pi++];
    auto name=StringRef(n)=="hostClock"?"clock":StringRef(n)=="hostReset"?"reset":n;
    require(q.name.getValue()==name&&p.type==q.type&&p.direction==q.direction&&p.annotations==q.annotations&&p.sym==q.sym,"public port identity");
    auto child="sim."+std::string(n);
    require(got.at(p.direction==Direction::In?child:std::string(name))==(p.direction==Direction::In?std::string(name):child),"public forwarding connection");
  }
  require(got.size()==5,"only public connections expected");
  auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(ann.size()==oldCount+1,"annotation count");
  auto src=cast<DictionaryAttr>(ann[ann.size()-1]);
  require(src.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::HostClockSource&&src.getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>clock","host clock source contract");
  unsigned clock=0,reset=0;
  for(auto a:ann) {
    auto d=cast<DictionaryAttr>(a);auto cls=d.getAs<StringAttr>("class").getValue();
    if(cls==goldengate::AnnotationClasses::HostClock){require(d.getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>clock","clock transfer");++clock;}
    if(cls==goldengate::AnnotationClasses::HostReset){require(d.getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>reset","reset transfer");++reset;}
  }
  require(clock==1&&reset==1,"unique FAME host identities");
}
void apply(CircuitOp c) {
  auto count=c->getAttrOfType<ArrayAttr>("rawAnnotations").size();std::string error;
  std::map<std::string,std::string> bodies;for(auto m:c.getOps<FModuleOp>())bodies[m.getName().str()]=dump(m);
  require(succeeded(goldengate::assembleFPGATopShell(c,error)),error);require(succeeded(verify(c)),"invalid shell IR");check(c,count);
  for(auto [n,s]:bodies)require(dump(top(c,n))==s,"old module body changed");
}
// Emit only the actual new wrapper and an external declaration for its child.
// This is a boundary lowering fixture, not a replacement simulator candidate.
void writeStub(CircuitOp c,StringRef name) {
  std::error_code ec;llvm::raw_fd_ostream os(name,ec);require(!ec,"cannot write shell fixture");
  auto child=dump(top(c,"GGCPUStreamPlatformWrapper"));child=child.substr(0,child.find(" {"));
  replace(child,"firrtl.module @","firrtl.extmodule private @");
  // External ports use names rather than SSA block arguments.
  child.erase(std::remove(child.begin(),child.end(),'%'),child.end());
  child+=" attributes {defname = \"GGCPUStreamPlatformWrapper\"}";
  os<<"module { firrtl.circuit \"FPGATop\" {\n"<<child<<"\n";top(c,"FPGATop")->print(os);os<<"\n} }\n";
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();apply(c);
    auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(cast<DictionaryAttr>(ann[2]).getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>ctrl.aw.bits.addr","aggregate reference transfer");
    require(cast<DictionaryAttr>(ann[5]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGCPUStreamPlatformWrapper>ctrl_diagnostic","diagnostic identity retention");
    require(cast<DictionaryAttr>(ann[6]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGCPUStreamPlatformWrapper>ep_clock","bridge clock identity retention");
    for(unsigned mode=1;mode<=18;++mode) {
      auto test=fixture(ctx,mode);auto tc=*test->getOps<CircuitOp>().begin();auto t=top(tc,"GGCPUStreamPlatformWrapper");OpBuilder b(&ctx);
      auto raw=tc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> as(raw.begin(),raw.end());
      if(mode==9)tc->removeAttr("rawAnnotations");
      if(mode==10)tc.setName("Wrong");
      if(mode==11){b.setInsertionPointToEnd(tc.getBodyBlock());b.create<FModuleOp>(tc.getLoc(),b.getStringAttr("FPGATop"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
      if(mode==12){as.erase(as.begin());tc->setAttr("rawAnnotations",b.getArrayAttr(as));}
      if(mode==13){as.push_back(as[0]);tc->setAttr("rawAnnotations",b.getArrayAttr(as));}
      if(mode==14){auto d=cast<DictionaryAttr>(as[1]);NamedAttrList a(d);a.set("target",b.getStringAttr("~Other|Other>reset"));as[1]=a.getDictionary(&ctx);tc->setAttr("rawAnnotations",b.getArrayAttr(as));}
      if(mode==15){as.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::HostClockSource))}));tc->setAttr("rawAnnotations",b.getArrayAttr(as));}
      if(mode==16){b.setInsertionPointToStart(top(tc,"GGHostMemoryPlatformWrapper").getBodyBlock());b.create<InstanceOp>(tc.getLoc(),t,"nested");}
      if(mode==17||mode==18){for(auto conn:t.getOps<ConnectOp>())if(path(t,conn.getDest())=="sim.hostClock"){if(mode==17)conn.erase();else{b.setInsertionPoint(conn);b.create<ConnectOp>(conn.getLoc(),conn.getDest(),conn.getSrc());}break;}}
      auto s=dump(*test);require(failed(goldengate::assembleFPGATopShell(tc,error))&&!error.empty(),"invalid boundary accepted");require(s==dump(*test),"rejection mutated IR");
    }
    auto s=dump(*root);require(failed(goldengate::assembleFPGATopShell(c,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"Five public identity bindings, retained private scalar/bundle/clock targets; HostClockSource added; 19 atomic rejections\n";
    if(argc>=3) {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real parse");auto rc=*real->getOps<CircuitOp>().begin();apply(rc);
      llvm::outs()<<"Real Rocket shell: 5 ports, "<<rc->getAttrOfType<ArrayAttr>("rawAnnotations").size()<<" annotations; old module bodies unchanged\n";
      if(argc>=2)writeStub(rc,argv[1]);
    } else if(argc>=2)writeStub(c,argv[1]);
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
