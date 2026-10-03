// See LICENSE for license details.
#include "goldengate/FPGATopShell.h"
#include "goldengate/F1Shim.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/AnnotationEmission.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
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
void applyShim(CircuitOp c) {
  std::string error;
  std::map<std::string,std::string> bodies;for(auto m:c.getOps<FModuleOp>())bodies[m.getName().str()]=dump(m);
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::assembleF1Shim(c,error)),error);
  require(succeeded(verify(c)),"invalid shim IR");
  auto shim=top(c,"F1Shim");
  require(c.getName()=="F1Shim"&&shim.getNumPorts()==8,"shim schema");
  unsigned regs=0;
  for(auto r:shim.getOps<RegResetOp>()) {
    require(cast<UIntType>(r.getResult().getType()).getWidth()==12,"request counter width");
    require(r.getClockVal()==shim.getBodyBlock()->getArgument(0)&&r.getResetSignal()==shim.getBodyBlock()->getArgument(1),"counter clock/reset");
    ++regs;
  }
  require(regs==2&&shim.getOps<MemOp>().empty()&&shim.getOps<RegOp>().empty(),"only two synchronous reset counters");
  unsigned instances=0;for(auto i:shim.getOps<InstanceOp>()){require(i.getModuleName()=="FPGATop","shim child");++instances;}
  require(instances==1,"unique platform child");
  for(auto [n,s]:bodies)require(dump(top(c,n))==s,"retained body changed");
  auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(ann.size()==raw.size()+2,"DDR/XDC path annotation count");
  auto output=cast<DictionaryAttr>(ann[ann.size()-2]);
  require(output.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::OutputFile&&
          output.getAs<StringAttr>("fileSuffix").getValue()==".defines.vh"&&!output.get("target"),"DDR output annotation schema");
  auto paths=cast<DictionaryAttr>(ann[ann.size()-1]);
  require(paths.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::XDCPaths&&
          paths.getAs<StringAttr>("preLinkPath").getValue()=="firesim_top"&&
          paths.getAs<StringAttr>("postLinkPath").getValue()=="firesim_top","U250 XDC paths");
  unsigned sources=0;for(auto a:ann){auto d=cast<DictionaryAttr>(a);if(d.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::HostClockSource){require(d.getAs<StringAttr>("target").getValue()=="~F1Shim|FPGATop>clock","retained host clock identity");++sources;}}
  require(sources==1,"host source count");
}
void emitCollateral(CircuitOp c,StringRef boundary) {
  auto directory=llvm::sys::path::parent_path(boundary);
  std::string error;auto before=dump(c);
  require(succeeded(goldengate::emitOutputFiles(c,directory,"FireSim-generated",error)),error);
  require(before==dump(c),"emission changed retained IR");
  llvm::SmallString<256> path(directory);llvm::sys::path::append(path,"FireSim-generated.defines.vh");
  auto bytes=llvm::MemoryBuffer::getFile(path);require(bool(bytes),"output not written");
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  for(unsigned mode=0;mode<5;++mode) {
    SmallVector<Attribute> annotations(raw.begin(),raw.end());
    unsigned index=annotations.size()-2;
    OpBuilder b(c.getContext());NamedAttrList output(cast<DictionaryAttr>(annotations[index]));
    if(mode==0)annotations.push_back(annotations[index]);
    if(mode==1){output.erase("body");annotations[index]=output.getDictionary(c.getContext());}
    if(mode==2){output.set("fileSuffix",b.getStringAttr(".vh/../../escape"));annotations[index]=output.getDictionary(c.getContext());}
    if(mode==3){output.set("target",b.getStringAttr("~F1Shim"));annotations[index]=output.getDictionary(c.getContext());}
    c->setAttr("rawAnnotations",b.getArrayAttr(annotations));
    auto invalid=dump(c);
    require(failed(goldengate::emitOutputFiles(c,directory,mode==4?"../escape":"FireSim-generated",error)),"invalid output accepted");
    require(invalid==dump(c),"invalid emission changed IR");
    auto retained=llvm::MemoryBuffer::getFile(path);
    require(bool(retained)&&retained.get()->getBuffer()==bytes.get()->getBuffer(),"invalid emission overwrote output");
  }
  c->setAttr("rawAnnotations",raw);
  llvm::SmallString<256> json(directory);llvm::sys::path::append(json,"f1-shim-all.json");
  require(succeeded(goldengate::emitAllAnnotations(c,json,error)),error);
  llvm::outs()<<"DDR collateral emitted; five invalid file sets rejected before writes\n";
}
void writeStub(CircuitOp c,StringRef name) {
  std::error_code ec;llvm::raw_fd_ostream os(name,ec);require(!ec,"cannot write fixture");
  auto child=dump(top(c,"FPGATop"));child=child.substr(0,child.find(" {"));
  replace(child,"firrtl.module @","firrtl.extmodule private @");
  child.erase(std::remove(child.begin(),child.end(),'%'),child.end());
  child+=" attributes {defname = \"FPGATop\"}";
  os<<"module { firrtl.circuit \"F1Shim\" {\n"<<child<<"\n";
  top(c,"F1Shim")->print(os);os<<"\n} }\n";
}
} // namespace
int main(int argc,char **argv) {
  try {
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::chirrtl::CHIRRTLDialect,circt::hw::HWDialect>();std::string error;
    auto make=[&](){auto r=fixture(ctx);auto c=*r->getOps<CircuitOp>().begin();require(succeeded(goldengate::assembleFPGATopShell(c,error)),error);return r;};
    auto root=make();auto c=*root->getOps<CircuitOp>().begin();applyShim(c);
    for(unsigned mode=0;mode<11;++mode) {
      auto test=make();auto tc=*test->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto m=top(tc,"FPGATop");
      if(mode==0)tc.setName("Wrong");
      if(mode==1)tc->removeAttr("rawAnnotations");
      if(mode==2){auto as=tc->getAttrOfType<ArrayAttr>("rawAnnotations");tc->setAttr("rawAnnotations",b.getArrayAttr(as.getValue().drop_back()));}
      if(mode==3){b.setInsertionPointToEnd(tc.getBodyBlock());b.create<FModuleOp>(tc.getLoc(),b.getStringAttr("F1Shim"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});}
      if(mode==4){b.setInsertionPointToStart(top(tc,"GGCPUStreamPlatformWrapper").getBodyBlock());b.create<InstanceOp>(tc.getLoc(),m,"nested");}
      if(mode==5){auto as=tc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> v(as.begin(),as.end());v.push_back(v.back());tc->setAttr("rawAnnotations",b.getArrayAttr(v));}
      if(mode==6){auto as=tc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> v(as.begin(),as.end());NamedAttrList d(cast<DictionaryAttr>(v.back()));d.set("target",b.getStringAttr("~FPGATop|Other>clock"));v.back()=d.getDictionary(&ctx);tc->setAttr("rawAnnotations",b.getArrayAttr(v));}
      if(mode==9){auto as=tc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> v(as.begin(),as.end());v.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),b.getNamedAttr("fileSuffix",b.getStringAttr(".defines.vh"))}));tc->setAttr("rawAnnotations",b.getArrayAttr(v));}
      if(mode==10){auto as=tc->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<Attribute> v(as.begin(),as.end());v.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::XDCPaths))}));tc->setAttr("rawAnnotations",b.getArrayAttr(v));}
      if(mode==7||mode==8){auto text=dump(*test);std::string from=mode==7?"uint<25>":"!firrtl.clock",to=mode==7?"uint<26>":"!firrtl.uint<1>";while(text.find(from)!=std::string::npos)replace(text,from,to);test=parseSourceString<ModuleOp>(text,&ctx);require(bool(test),"bad schema fixture parse");tc=*test->getOps<CircuitOp>().begin();}
      auto before=dump(*test);require(failed(goldengate::assembleF1Shim(tc,error))&&!error.empty(),"invalid shim accepted");require(before==dump(*test),"rejection changed IR");
    }
    auto before=dump(*root);require(failed(goldengate::assembleF1Shim(c,error)),"repeat accepted");require(before==dump(*root),"repeat changed IR");
    llvm::outs()<<"U250 F1Shim, two 12-bit host counters, retained bodies/clock identity; 12 atomic rejections\n";
    if(argc>=3){auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real parse");auto rc=*real->getOps<CircuitOp>().begin();applyShim(rc);writeStub(rc,argv[1]);emitCollateral(rc,argv[1]);llvm::outs()<<"Real Rocket F1Shim: 8 aggregate ports; old bodies retained; DDR output annotation added\n";}
    else if(argc>=2){writeStub(c,argv[1]);emitCollateral(c,argv[1]);}
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
