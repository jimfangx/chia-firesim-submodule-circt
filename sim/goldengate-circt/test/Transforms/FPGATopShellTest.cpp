// See LICENSE for license details.
#include "goldengate/FPGATopShell.h"
#include "goldengate/CPUStreamRead.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned mode=0,
                            StringRef diagnosticType="", bool isF2=false,
                            StringRef diagnosticName="ctrl_diagnostic", bool noCPU=false) {
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
  if(mode==19)replace(source,"addr: uint<25>, len", "addr: vector<uint<25>, 1>, len");
  if(mode==20)replace(source,"addr: uint<25>, len", "unexpected: vector<uint<1>, 0>, addr: uint<25>, len");
  if(!diagnosticType.empty())replace(source,"out %ctrl_diagnostic: !firrtl.uint<4>","out %ctrl_diagnostic: "+diagnosticType.str());
  if(diagnosticName!="ctrl_diagnostic")replace(source,"ctrl_diagnostic",diagnosticName);
  if(isF2)for(unsigned i=0;i<2;++i)replace(source,"addr: uint<25>","addr: uint<32>");
  auto root=parseSourceString<ModuleOp>(source,&ctx);require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);auto m=top(c,"GGCPUStreamPlatformWrapper");
  if (noCPU) {
    llvm::BitVector removed(m.getNumPorts());
    for (unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)=="cpu_managed_axi4") removed.set(i);
    m.erasePorts(removed); m.setName("GGHostMemoryPlatformWrapper"); c.setName("GGHostMemoryPlatformWrapper");
  }
  const std::string boundary = c.getName().str();
  SmallVector<Attribute> annotations;
  for(auto [cls,n]:{std::pair<StringRef,StringRef>{goldengate::AnnotationClasses::HostClock,"hostClock"},
                   {goldengate::AnnotationClasses::HostReset,"hostReset"},
                   {"test.Target","ctrl.aw.bits.addr"},{"test.Target","mem_0"},
                   {"test.Target","cpu_managed_axi4.r.bits.data"},
                   {"test.Target","ctrl_diagnostic"},{"test.Target","ep_clock"}}) {
    if (noCPU && n.starts_with("cpu_managed_axi4")) continue;
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(cls)),b.getNamedAttr("target",b.getStringAttr("~"+boundary+"|"+boundary+">"+(n=="ctrl_diagnostic"?diagnosticName:n).str()))}));
  }
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations));
  SmallVector<PortInfo> simPorts,cpuPorts;
  for(auto p:m.getPorts())if(p.name.getValue()==(noCPU ? "mem_0" : "cpu_managed_axi4"))cpuPorts.push_back(p);else simPorts.push_back(p);
  if (noCPU) cpuPorts[0].name=b.getStringAttr("out");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto child=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(noCPU ? "GGHostMemoryReadResponseWrapper" : "GGHostMemoryPlatformWrapper"),ConventionAttr::get(&ctx,Convention::Internal),simPorts);
  auto helper=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(noCPU ? "GGHostMemoryPortAdapter" : "GGCPUStreamPortAdapter"),ConventionAttr::get(&ctx,Convention::Internal),cpuPorts);
  b.setInsertionPointToStart(m.getBodyBlock());auto sim=b.create<InstanceOp>(c.getLoc(),child,"sim"),cpu=b.create<InstanceOp>(c.getLoc(),helper,noCPU ? "memory_port" : "cpu_port");
  unsigned k=0;
  for(auto [i,p]:llvm::enumerate(m.getPorts())) {
    Value a=m.getBodyBlock()->getArgument(i),v=p.name.getValue()==(noCPU ? "mem_0" : "cpu_managed_axi4")?cpu.getResult(0):sim.getResult(k++);
    b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In?v:a,p.direction==Direction::In?a:v);
  }
  return root;
}
std::string path(FModuleOp m,Value v) {
  if(auto a=dyn_cast<BlockArgument>(v))return m.getPortName(a.getArgNumber()).str();
  auto i=v.getDefiningOp<InstanceOp>();require(bool(i),"unknown root");
  return i.getName().str()+"."+i.getPortName(cast<OpResult>(v).getResultNumber()).str();
}
void check(CircuitOp c,unsigned oldCount,StringRef boundary="GGCPUStreamPlatformWrapper") {
  auto m=top(c,"FPGATop"),old=top(c,boundary);
  require(c.getName()=="FPGATop"&&m.getNumPorts()==5,"public shell must have exactly five ports");
  unsigned instances=0;
  for(auto i:m.getOps<InstanceOp>()) {
    require(i.getModuleName()==old.getName(),"shell must retain original inner module");
    ++instances;
  }
  require(instances==1,"shell must contain exactly one retained child");
  unsigned operations=0;
  for(auto &op:*m.getBodyBlock()) {
    require(isa<InstanceOp,ConnectOp>(op),"shell must contain only its instance and five identity connections");
    ++operations;
  }
  require(operations==6,"shell must contain only its instance and five identity connections");
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
  unsigned clock=0,reset=0,sources=0;
  for(auto a:ann) {
    auto d=cast<DictionaryAttr>(a);auto cls=d.getAs<StringAttr>("class").getValue();
    if(cls==goldengate::AnnotationClasses::HostClock){require(d.getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>clock","clock transfer");++clock;}
    if(cls==goldengate::AnnotationClasses::HostReset){require(d.getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>reset","reset transfer");++reset;}
    if(cls==goldengate::AnnotationClasses::HostClockSource)++sources;
  }
  require(clock==1&&reset==1&&sources==1,"unique FAME host identities and clock source");
}
void apply(CircuitOp c,bool print=false,bool isF2=false) {
  auto count=c->getAttrOfType<ArrayAttr>("rawAnnotations").size();std::string error;
  std::map<std::string,std::string> bodies;for(auto m:c.getOps<FModuleOp>())bodies[m.getName().str()]=dump(m);
  auto boundary=print?"GGPrintCPUStreamPlatformWrapper":"GGCPUStreamPlatformWrapper";
  auto applied=goldengate::attachFPGATopShell(c,boundary,error,isF2);
  require(succeeded(applied),error);require(succeeded(verify(c)),"invalid shell IR");check(c,count,boundary);
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
    for (bool isF2 : {false,true}) {
      auto test=fixture(ctx,0,"",isF2,"ctrl_diagnostic",true);
      auto nc=*test->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::assembleFPGATopShell(nc,error,isF2)),error);
      require(succeeded(verify(*test)),"invalid no-stream FPGATop");
      require(top(nc,"FPGATop").getNumPorts()==4,"no-stream shell must omit CPU port");
    }
    if(argc==4&&StringRef(argv[1])=="--apply-f2-input") {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"F2 platform input parse");
      require(succeeded(verify(*real)),"invalid F2 platform input IR");
      auto circuit=*real->getOps<CircuitOp>().begin();
      require(succeeded(goldengate::enableF2CPUStreamWordReads(circuit,error)),error);
      require(succeeded(goldengate::assembleFPGATopShell(circuit,error,true)),error);
      require(succeeded(verify(*real)),"invalid F2 platform shell IR");
      std::error_code ec;llvm::raw_fd_ostream out(argv[3],ec);require(!ec,"cannot write F2 shell output");
      real->print(out);out<<'\n';
      llvm::outs()<<"PASS exact production F2 platform input through wrapper assembly\n";return 0;
    }
    // Reuse actual post-MMIO FRFCFS diagnostic types from the component
    // pipeline. The public shell/forwarding is still a boundary scaffold;
    // this is not a complete target candidate or semantic comparison.
    if(argc==3&&StringRef(argv[1])=="--frfcfs-diagnostics") {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"FRFCFS fixture parse");
      require(succeeded(verify(*real)),"invalid FRFCFS fixture IR");
      auto rc=*real->getOps<CircuitOp>().begin();unsigned vectors=0;
      for(auto p:top(rc,rc.getName()).getPorts()) {
        if(!p.name.getValue().starts_with("fased_frfcfs_")||!isa<FVectorType>(p.type))continue;
        require(p.direction==Direction::Out,"FRFCFS vector diagnostic is an input");
        std::string type;llvm::raw_string_ostream os(type);p.type.print(os);
        auto test=fixture(ctx,0,type,true,p.name.getValue());auto tc=*test->getOps<CircuitOp>().begin();apply(tc,false,true);
        llvm::outs()<<p.name.getValue()<<": "<<type<<"\n";++vectors;
      }
      require(vectors==4,"expected four retained FRFCFS vector diagnostics after MMIO");
      llvm::outs()<<"PASS four production FRFCFS diagnostic types through F2 boundary scaffold\n";return 0;
    }
    // Inspect the actual production boundary without applying the pass again
    // or synthesizing FAME identities. Exact aggregate SSA identities prove
    // all payload/handshake bits forward independently of their values.
    if(argc>=3&&StringRef(argv[1])=="--platform-boundary") {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"production shell parse");
      require(succeeded(verify(*real)),"invalid production shell IR");
      auto rc=*real->getOps<CircuitOp>().begin();
      auto raw=rc->getAttrOfType<ArrayAttr>("rawAnnotations");require(raw&&raw.size(),"missing annotation archive");
      check(rc,raw.size()-1);
      if(argc==4)writeStub(rc,argv[3]);
      llvm::outs()<<"Production shell: five unconditional aggregate identities, no added state/clock logic, unique host clock/reset/source\n";
      return 0;
    }
    auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();apply(c);
    auto ann=c->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(cast<DictionaryAttr>(ann[2]).getAs<StringAttr>("target").getValue()=="~FPGATop|FPGATop>ctrl.aw.bits.addr","aggregate reference transfer");
    require(cast<DictionaryAttr>(ann[5]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGCPUStreamPlatformWrapper>ctrl_diagnostic","diagnostic identity retention");
    require(cast<DictionaryAttr>(ann[6]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGCPUStreamPlatformWrapper>ep_clock","bridge clock identity retention");
    // Vector traversal must preserve aggregate flips at every nesting level.
    // Exercise both exact public platform contracts and retain private targets.
    for(bool isF2:{false,true}) {
      for(StringRef type:{"!firrtl.vector<uint<32>, 5>",
                          "!firrtl.vector<bundle<state: uint<1>, banks: vector<bundle<openRow: uint<26>>, 8>>, 4>"}) {
        auto test=fixture(ctx,0,type,isF2);auto tc=*test->getOps<CircuitOp>().begin();apply(tc,false,isF2);
        auto raw=tc->getAttrOfType<ArrayAttr>("rawAnnotations");
        require(cast<DictionaryAttr>(raw[5]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGCPUStreamPlatformWrapper>ctrl_diagnostic","vector diagnostic identity lost");
      }
      for(StringRef type:{"!firrtl.vector<bundle<banks: vector<bundle<ready flip: uint<1>>, 8>>, 4>",
                          "!firrtl.bundle<lanes flip: vector<uint<1>, 4>>",
                          "!firrtl.vector<clock, 4>",
                          "!firrtl.vector<reset, 4>",
                          "!firrtl.vector<uint, 4>"}) {
        auto test=fixture(ctx,0,type,isF2);auto tc=*test->getOps<CircuitOp>().begin();auto before=dump(*test);
        require(failed(goldengate::assembleFPGATopShell(tc,error,isF2))&&before==dump(*test),"unsafe vector diagnostic accepted or rejection mutated IR");
        require(error.find("ctrl_diagnostic")!=std::string::npos,"diagnostic error omits failing port/leaf");
      }
    }
    // Synthesized Print domain clocks are private only when their actual
    // forwarding reaches the clock named by retained channel metadata.
    for (unsigned mode=0; mode<5; ++mode) {
      auto test=fixture(ctx,0,"!firrtl.clock",true,"synthesizedPrintf_domain_clock");
      auto tc=*test->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
      auto raw=tc->getAttrOfType<ArrayAttr>("rawAnnotations");
      SmallVector<Attribute> annotations(raw.begin(),raw.end());
      if (mode != 1) annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("clock", b.getStringAttr("~GGCPUStreamPlatformWrapper|GGHostMemoryPlatformWrapper>" +
          std::string(mode==2 ? "ep_clock" : "synthesizedPrintf_domain_clock")))}));
      tc->setAttr("rawAnnotations",b.getArrayAttr(annotations));
      auto inner=top(tc,"GGCPUStreamPlatformWrapper");
      for (auto connection:llvm::make_early_inc_range(inner.getOps<ConnectOp>()))
        if (path(inner,connection.getDest())=="synthesizedPrintf_domain_clock") {
          if (mode==3) connection.erase();
          else if (mode==4) {
            b.setInsertionPoint(connection);
            b.create<StrictConnectOp>(connection.getLoc(),connection.getDest(),connection.getSrc());
          }
          break;
        }
      if (!mode) apply(tc,false,true);
      else {
        auto before=dump(*test);
        require(failed(goldengate::assembleFPGATopShell(tc,error,true))&&before==dump(*test),
          "unproven Print clock alias accepted or rejection mutated IR");
      }
    }
    for(unsigned mode=1;mode<=20;++mode) {
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
    // The expanded profile accepts additional passive payload diagnostics.
    // Host annotations remain mandatory; no identities are synthesized.
    auto expanded=fixture(ctx); auto ec=*expanded->getOps<CircuitOp>().begin();
    auto source=dump(*expanded);
    const std::pair<StringRef,StringRef> replacements[]{
      {"GGCPUStreamPlatformWrapper","GGPrintCPUStreamPlatformWrapper"},
      {"GGHostMemoryPlatformWrapper","GGPrintCPUStreamWriteResponseBufferWrapper"},
      {"ctrl_diagnostic","payload_probe"}};
    for(auto [from,to]:replacements) {
      size_t pos=0;while((pos=source.find(from.str(),pos))!=std::string::npos){source.replace(pos,from.size(),to.str());pos+=to.size();}
    }
    expanded=parseSourceString<ModuleOp>(source,&ctx);require(bool(expanded),"expanded parse");
    ec=*expanded->getOps<CircuitOp>().begin();
    // A strict competing driver does not increase the public argument's use
    // count, but must still reject both supported boundary profiles atomically.
    for(bool print:{false,true}) {
      auto test=print?OwningOpRef<ModuleOp>(cast<ModuleOp>(expanded->clone())):fixture(ctx);
      auto tc=*test->getOps<CircuitOp>().begin();auto boundary=print?"GGPrintCPUStreamPlatformWrapper":"GGCPUStreamPlatformWrapper";
      auto inner=top(tc,boundary);OpBuilder b(&ctx);
      for(auto conn:inner.getOps<ConnectOp>())if(path(inner,conn.getDest())=="sim.hostClock") {
        b.setInsertionPoint(conn);auto other=b.create<WireOp>(conn.getLoc(),ClockType::get(&ctx),"otherClock");
        b.create<StrictConnectOp>(conn.getLoc(),conn.getDest(),other.getResult());break;
      }
      auto before=dump(*test);require(failed(goldengate::attachFPGATopShell(tc,boundary,error))&&before==dump(*test),"strict competing driver accepted or mutated IR");
    }
    // Exact forwarding via strict connects is also a supported input shape.
    auto ei=top(ec,"GGPrintCPUStreamPlatformWrapper");OpBuilder eb(&ctx);
    for(auto conn:llvm::make_early_inc_range(ei.getOps<ConnectOp>())) {
      // Strict connects require passive types; AXI aggregates contain flips.
      if(!isa<ClockType,UIntType>(conn.getDest().getType()))continue;
      eb.setInsertionPoint(conn);eb.create<StrictConnectOp>(conn.getLoc(),conn.getDest(),conn.getSrc());conn.erase();
    }
    apply(ec,true);
    auto ea=ec->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(cast<DictionaryAttr>(ea[5]).getAs<StringAttr>("target").getValue()=="~FPGATop|GGPrintCPUStreamPlatformWrapper>payload_probe","Print passive diagnostic target lost");
    auto s=dump(*root);require(failed(goldengate::assembleFPGATopShell(c,error)),"repeat accepted");require(s==dump(*root),"repeat mutated IR");
    llvm::outs()<<"Five public identity bindings, retained private scalar/bundle/vector/clock targets; HostClockSource added; 33 atomic rejections; U250/F2 passive vectors and expanded diagnostic profile\n";
    if(argc>=3) {
      auto real=parseSourceFile<ModuleOp>(argv[2],&ctx);require(bool(real),"real parse");auto rc=*real->getOps<CircuitOp>().begin();apply(rc);
      llvm::outs()<<"Real Rocket shell: 5 ports, "<<rc->getAttrOfType<ArrayAttr>("rawAnnotations").size()<<" annotations; old module bodies unchanged\n";
      if(argc>=2)writeStub(rc,argv[1]);
    } else if(argc>=2)writeStub(c,argv[1]);
    return 0;
  }catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}
}
