// See LICENSE for license details.
#include "goldengate/FASEDWriteRetirement.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
bool selected = false;
std::string wrapperName() { return selected ? "GGSelectedPrintFASEDWriteRetirementWrapper" : "GGFASEDWriteRetirementWrapper"; }
std::string moduleName(llvm::StringRef suffix) { return (selected ? "GGSelectedPrintFASED" : "GGFASED") + suffix.str() + "Wrapper"; }
SmallVector<std::string> names;
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m:c.getOps<FModuleOp>()) if (m.getName()==n) return m;
  throw std::runtime_error("missing module");
}
Value driver(FModuleOp m, Value v) {
  Value found;
  for (auto &op:*m.getBodyBlock()) if (isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(0)==v) {
    require(!found,"multiple drivers");found=op.getOperand(1);
  }
  require(bool(found),"missing driver");return found;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"" + names[0] + "\" { firrtl.module @" + names[0] + "() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto bit=UIntType::get(&ctx,1,false);auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  auto responseBits=BundleType::get(&ctx,{{b.getStringAttr("user"),false,bit},
    {b.getStringAttr("id"),false,uint(mode==6?5:4)},{b.getStringAttr("resp"),false,uint(2)}});
  auto response=BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},
    {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,responseBits}});
  SmallVector<PortInfo> base{{b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("targetFire"),bit,Direction::In},
    {b.getStringAttr("echo"),bit,Direction::Out}};
  auto make=[&](llvm::StringRef name,ArrayRef<PortInfo> ports){
    b.setInsertionPointToEnd(c.getBodyBlock());return b.create<FModuleOp>(loc,b.getStringAttr(name),
      ConventionAttr::get(&ctx,Convention::Internal),ports);
  };
  auto rp=base;rp.push_back({b.getStringAttr(mode==5?"missing":"b"),response,mode==7?Direction::In:Direction::Out});
  auto releaser=make("GGFASEDResponseReleaser",rp);
  FModuleOp inner;
  for (unsigned j=6;j-- >0;) {
    auto ports=base;
    if(j==0)ports.push_back({b.getStringAttr(mode==2?"missing":"fased_target_b_fire"),
      mode==3?uint(2):bit,mode==4?Direction::Out:Direction::In});
    if(mode==10+j)ports.push_back({b.getStringAttr("fased_accepted_b_fire"),bit,Direction::Out});
    auto m=make(names[j],ports);b.setInsertionPointToStart(m.getBodyBlock());
    auto inst=b.create<InstanceOp>(loc,inner?inner:releaser,j==5?(mode==8?"wrong":"releaser"):(mode==9?"wrong":"sim"));
    inst->setAttr("test.preserved",b.getStringAttr("identity"));
    for(unsigned i=0;i<base.size();++i)b.create<StrictConnectOp>(loc,
      base[i].direction==Direction::In?inst.getResult(i):m.getBodyBlock()->getArgument(i),
      base[i].direction==Direction::In?m.getBodyBlock()->getArgument(i):inst.getResult(i));
    inner=m;
  }
  if(mode==16) {b.setInsertionPointToEnd(inner.getBodyBlock());b.create<InstanceOp>(loc,named(c,names[3]),"shared");}
  if(mode==17) {auto other=make("Other",{});b.setInsertionPointToEnd(other.getBodyBlock());b.create<InstanceOp>(loc,inner,"used");}
  if(mode==18)make(wrapperName(),{});
  if(mode==19)c.setName("Wrong");
  if(mode!=1)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({
      b.getStringAttr("~" + names[0] + "|" + names[0] + ">fased_target_b_fire"),
      b.getStringAttr("~" + names[0] + "|" + names[0] + ">echo"),
      b.getStringAttr("~" + names[0] + "|" + names[4] + "/sim:" + names[5] + ">echo")}))})}));
  return root;
}
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
  auto oldTop=named(c,names[0]);auto oldPorts=oldTop.getPorts();std::string error;
  require(succeeded(goldengate::bindFASEDWriteRetirement(c,error)),error);
  require(succeeded(verify(*root)),"invalid retirement IR");
  auto top=named(c,c.getName());require(top.getNumPorts()==oldPorts.size()-1,"external retirement input not consumed");
  auto sim=*top.getOps<InstanceOp>().begin();
  require(driver(top,sim.getResult(4))==sim.getResult(5),"counter feedback not closed");
  for(unsigned j=0;j<4;++j) {
    require(top.getPortName(j)==oldPorts[j].name&&top.getPortType(j)==oldPorts[j].type&&top.getPortDirection(j)==oldPorts[j].direction,"copied port changed");
    require(driver(top,j==3?top.getBodyBlock()->getArgument(j):sim.getResult(j))==
      (j==3?sim.getResult(j):top.getBodyBlock()->getArgument(j)),"copied connection changed");
  }
  Value observed;
  for(unsigned j=0;j<6;++j) {
    auto m=named(c,names[j]);auto child=*m.getOps<InstanceOp>().begin();
    auto sentinel=child->getAttrOfType<StringAttr>("test.preserved");
    require(sentinel&&sentinel.getValue()=="identity","instance attribute lost");
    for(unsigned i=0;i<3;++i)
      require(driver(m,child.getResult(i))==m.getBodyBlock()->getArgument(i),"existing input use lost");
    require(driver(m,m.getBodyBlock()->getArgument(3))==child.getResult(3),"existing output use lost");
    require(m.getPortName(m.getNumPorts()-1)=="fased_accepted_b_fire","observation absent");
    auto v=driver(m,m.getBodyBlock()->getArgument(m.getNumPorts()-1));
    if(j<5)require(v==child.getResult(child.getNumResults()-1),"observation propagation differs");
    else observed=v;
  }
  auto andOp=observed.getDefiningOp<AndPrimOp>();require(bool(andOp),"not a B handshake");
  auto ready=andOp.getLhs().getDefiningOp<SubfieldOp>(),valid=andOp.getRhs().getDefiningOp<SubfieldOp>();
  require(ready&&valid&&ready.getFieldIndex()==0&&valid.getFieldIndex()==1&&ready.getInput()==valid.getInput(),"wrong B fields");
  require(ready.getInput()==(*named(c,names[5]).getOps<InstanceOp>().begin()).getResult(4),"wrong response source");
  auto attrs=c->getAttrOfType<ArrayAttr>("rawAnnotations");auto targets=cast<DictionaryAttr>(attrs[0]).getAs<ArrayAttr>("targets");
  require(cast<StringAttr>(targets[0]).getValue()=="~" + wrapperName() + "|" + names[0] + ">fased_target_b_fire","consumed target moved");
  require(cast<StringAttr>(targets[1]).getValue()=="~" + wrapperName() + "|" + wrapperName() + ">echo","copied target not moved");
  require(cast<StringAttr>(targets[2]).getValue()=="~" + wrapperName() + "|" + names[4] + "/sim:" + names[5] + ">echo","hierarchy target changed");
  llvm::outs()<<"Six response observation hops, closed retirement feedback, existing users and annotation targets passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=19;++mode) {
    auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::bindFASEDWriteRetirement(c,error))&&!error.empty(),"unsupported hierarchy accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
  require(succeeded(goldengate::bindFASEDWriteRetirement(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::bindFASEDWriteRetirement(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"20 atomic rejection cases passed\n";
}
unsigned port(FModuleOp m, llvm::StringRef name) {
  for (unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: " + name.str());
}
// Cut only at accepted AW/last-W, target B fields and model scheduling inputs.
// Check the complete observation path before cloning its actual AND operation
// and the actual helper bindings; do not generate a replacement counter body.
void exportProduction(MLIRContext &ctx, const char *input, const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production retirement boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()==wrapperName(),"expected retirement boundary");
  auto top=named(c,c.getName());auto sim=*top.getOps<InstanceOp>().begin();
  auto pairingWrapper=named(c,names[0]);
  require(sim.getModuleName()==names[0]&&
    driver(top,sim.getResult(port(pairingWrapper,"fased_target_b_fire")))==
      sim.getResult(port(pairingWrapper,"fased_accepted_b_fire")),"retirement feedback differs");
  Value observed;
  for(unsigned j=0;j<6;++j) {
    auto m=named(c,names[j]);auto v=driver(m,m.getBodyBlock()->getArgument(port(m,"fased_accepted_b_fire")));
    if(j<5) {
      auto child=v.getDefiningOp<InstanceOp>();auto next=named(c,names[j+1]);
      require(child&&child.getInstanceName()=="sim"&&child.getModuleName()==names[j+1]&&
        v==child.getResult(port(next,"fased_accepted_b_fire")),"observation hop differs");
    } else observed=v;
  }
  auto handshake=observed.getDefiningOp<AndPrimOp>();require(bool(handshake),"missing production B handshake");
  auto ready=handshake.getLhs().getDefiningOp<SubfieldOp>(),valid=handshake.getRhs().getDefiningOp<SubfieldOp>();
  require(ready&&valid&&ready.getFieldIndex()==0&&valid.getFieldIndex()==1&&ready.getInput()==valid.getInput(),"wrong production B fields");
  auto releaser=ready.getInput().getDefiningOp<InstanceOp>();
  require(releaser&&releaser.getInstanceName()=="releaser"&&releaser.getModuleName()=="GGFASEDResponseReleaser"&&
    ready.getInput()==releaser.getResult(port(named(c,"GGFASEDResponseReleaser"),"b")),"wrong production response source");
  auto helper=named(c,"GGFASEDWritePairing");InstanceOp original;
  for(auto i:pairingWrapper.getOps<InstanceOp>())if(i.getModuleName()==helper.getName())original=i;
  require(bool(original),"missing pending counters");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDWriteRetirementProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());b.insert(helper->clone());
  SmallVector<PortInfo> ports;
  for(auto p:helper.getPorts())if(p.name.getValue()!="bFire")ports.push_back(p);
  auto bit=UIntType::get(&ctx,1,false);
  ports.push_back({b.getStringAttr("bReady"),bit,Direction::In});
  ports.push_back({b.getStringAttr("bValid"),bit,Direction::In});
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDWriteRetirementProbe"),
    ConventionAttr::get(&ctx,Convention::Internal),ports);probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());
  auto pending=b.create<InstanceOp>(c.getLoc(),helper,"pairing");IRMapping map;
  for(unsigned i=0;i<helper.getNumPorts();++i)map.map(original.getResult(i),pending.getResult(i));
  map.map(ready.getResult(),probe.getBodyBlock()->getArgument(port(probe,"bReady")));
  map.map(valid.getResult(),probe.getBodyBlock()->getArgument(port(probe,"bValid")));
  Value accepted=b.clone(*handshake,map)->getResult(0);
  for(unsigned i=0;i<helper.getNumPorts();++i) {
    auto name=helper.getPortName(i);Value dest,src;
    if(helper.getPortDirection(i)==Direction::In) {
      dest=original.getResult(i);src=driver(pairingWrapper,dest);
      map.map(src,name=="bFire"?accepted:probe.getBodyBlock()->getArgument(port(probe,name)));
    } else {
      src=original.getResult(i);
      for(auto &op:*pairingWrapper.getBodyBlock())
        if(isa<StrictConnectOp,ConnectOp>(op)&&op.getOperand(1)==src) {
          require(!dest,"multiple helper output bindings");dest=op.getOperand(0);
        }
      require(bool(dest),"missing helper output binding");
      map.map(dest,probe.getBodyBlock()->getArgument(port(probe,name)));
    }
    bool copied=false;
    for(auto &op:*pairingWrapper.getBodyBlock())
      if(isa<StrictConnectOp,ConnectOp>(op)&&op.getOperand(0)==dest&&op.getOperand(1)==src) {
        b.clone(op,map);copied=true;
      }
    require(copied,"helper connection not cloned");
  }
  require(succeeded(verify(isolated)),"invalid production retirement probe");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export retirement probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production B handshake and pending counter bindings after validating six observation hops\n";
}
}
int main(int argc,char **argv) {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    require(argc==1||argc==3,"usage: goldengate-fased-write-retirement-test [boundary.mlir probe.mlir]");
    for (bool profile : {false, true}) {
      selected = profile; names.clear();
      for (auto suffix : {"WritePairing", "TimingAWQueue", "WriteLatency", "ReadLatency", "TimingCycle", "ResponseReleaser"})
        names.push_back(moduleName(suffix));
      if(argc==3){exportProduction(ctx,argv[1],argv[2]);return 0;}
      behavior(ctx); rejection(ctx);
    }
  }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
