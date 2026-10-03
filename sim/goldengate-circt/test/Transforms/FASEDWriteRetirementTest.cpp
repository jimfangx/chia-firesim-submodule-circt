// See LICENSE for license details.
#include "goldengate/FASEDWriteRetirement.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
const llvm::StringRef names[]{"GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
  "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper", "GGFASEDTimingCycleWrapper",
  "GGFASEDResponseReleaserWrapper"};
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
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDWritePairingWrapper\" { firrtl.module @GGFASEDWritePairingWrapper() {} } }",&ctx);
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
  if(mode==18)make("GGFASEDWriteRetirementWrapper",{});
  if(mode==19)c.setName("Wrong");
  if(mode!=1)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({
      b.getStringAttr("~GGFASEDWritePairingWrapper|GGFASEDWritePairingWrapper>fased_target_b_fire"),
      b.getStringAttr("~GGFASEDWritePairingWrapper|GGFASEDWritePairingWrapper>echo"),
      b.getStringAttr("~GGFASEDWritePairingWrapper|GGFASEDTimingCycleWrapper/sim:GGFASEDResponseReleaserWrapper>echo")}))})}));
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
  require(cast<StringAttr>(targets[0]).getValue()=="~GGFASEDWriteRetirementWrapper|GGFASEDWritePairingWrapper>fased_target_b_fire","consumed target moved");
  require(cast<StringAttr>(targets[1]).getValue()=="~GGFASEDWriteRetirementWrapper|GGFASEDWriteRetirementWrapper>echo","copied target not moved");
  require(cast<StringAttr>(targets[2]).getValue()=="~GGFASEDWriteRetirementWrapper|GGFASEDTimingCycleWrapper/sim:GGFASEDResponseReleaserWrapper>echo","hierarchy target changed");
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
}
int main() {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {behavior(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
