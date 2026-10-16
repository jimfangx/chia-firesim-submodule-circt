// See LICENSE for license details.
#include "goldengate/FASEDReadAdmission.h"
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
unsigned maxFlight = 10;
unsigned countWidth() { return maxFlight >= 16 ? 5 : 4; }
llvm::StringRef names[]{"GGFASEDWriteAdmissionWrapper", "GGFASEDWriteRetirementWrapper",
  "GGFASEDWritePairingWrapper", "GGFASEDTimingAWQueueWrapper",
  "GGFASEDWriteLatencyWrapper", "GGFASEDReadLatencyWrapper", "GGFASEDTimingCycleWrapper",
  "GGFASEDResponseReleaserWrapper"};
bool selectedPrint = false;
std::string profile(llvm::StringRef text) {
  std::string result = text.str();
  if (selectedPrint) {
    const llvm::StringRef wrappers[]{"WriteAdmission", "WriteRetirement", "WritePairing",
      "TimingAWQueue", "WriteLatency", "ReadLatency", "TimingCycle", "ResponseReleaser", "ReadAdmission"};
    for (auto name : wrappers) {
      auto old = "GGFASED" + name.str() + "Wrapper";
      auto replacement = "GGSelectedPrintFASED" + name.str() + "Wrapper";
      for (size_t pos = 0; (pos = result.find(old, pos)) != std::string::npos; pos += replacement.size())
        result.replace(pos, old.size(), replacement);
    }
  }
  return result;
}
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
  auto root=parseSourceString<ModuleOp>(profile("module { firrtl.circuit \"GGFASEDWriteAdmissionWrapper\" { firrtl.module @GGFASEDWriteAdmissionWrapper() {} } }"),&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto bit=UIntType::get(&ctx,1,false);auto uint=[&](unsigned w){return UIntType::get(&ctx,w,false);};
  auto payload=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fields)es.push_back({b.getStringAttr(n),false,uint(w)});
    return BundleType::get(&ctx,es);
  };
  auto dec=[&](BundleType bits,bool flip){return BundleType::get(&ctx,{{b.getStringAttr("ready"),flip,bit},
    {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});};
  auto response=dec(payload({{"user",1},{"id",mode==6?5u:4u},{"last",1},{"data",64},{"resp",2}}),true);
  auto address=payload({{"user",1},{"id",4},{"region",4},{"qos",4},{"prot",3},{"cache",4},
    {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",64}});
  auto requests=BundleType::get(&ctx,{{b.getStringAttr("aw"),false,dec(address,false)},
    {b.getStringAttr("w"),false,dec(data,false)},{b.getStringAttr("ar"),false,dec(address,true)}});
  SmallVector<PortInfo> base{{b.getStringAttr(mode==20?"wrong":"hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr(mode==18?"wrong":"fased_model_reset"),bit,Direction::Out},{b.getStringAttr(mode==19?"wrong":"fased_tfire"),bit,Direction::Out},
    {b.getStringAttr("echo"),bit,Direction::Out}};
  auto make=[&](llvm::StringRef name,ArrayRef<PortInfo> ports){
    b.setInsertionPointToEnd(c.getBodyBlock());return b.create<FModuleOp>(loc,b.getStringAttr(name),
      ConventionAttr::get(&ctx,Convention::Internal),ports);
  };
  auto rp=base;rp.push_back({b.getStringAttr(mode==5?"missing":"r"),response,mode==7?Direction::In:Direction::Out});
  auto releaser=make("GGFASEDResponseReleaser",rp);
  FModuleOp inner;
  for (unsigned j=8;j-- >0;) {
    auto ports=base;
    if(j==0)ports.push_back({b.getStringAttr(mode==2?"missing":"fased_timing_requests"),
      mode==3?Type(uint(2)):Type(requests),mode==4?Direction::In:Direction::Out});
    if(mode==10+j)ports.push_back({b.getStringAttr("fased_accepted_r_last_fire"),bit,Direction::Out});
    if(mode==21&&j==0)ports.push_back({b.getStringAttr("fased_read_max_reqs"),uint(4),Direction::In});
    if(mode==22&&j==0)ports.push_back({b.getStringAttr("fased_pending_reads"),uint(4),Direction::Out});
    auto m=make(names[j],ports);b.setInsertionPointToStart(m.getBodyBlock());
    auto inst=b.create<InstanceOp>(loc,inner?inner:releaser,j==7?(mode==8?"wrong":"releaser"):(mode==9?"wrong":"sim"));
    inst->setAttr("test.preserved",b.getStringAttr("identity"));
    for(unsigned i=0;i<base.size();++i)b.create<StrictConnectOp>(loc,
      base[i].direction==Direction::In?inst.getResult(i):m.getBodyBlock()->getArgument(i),
      base[i].direction==Direction::In?m.getBodyBlock()->getArgument(i):inst.getResult(i));
    inner=m;
  }
  if(mode==23) {b.setInsertionPointToEnd(inner.getBodyBlock());b.create<InstanceOp>(loc,named(c,names[3]),"shared");}
  if(mode==24) {auto other=make("Other",{});b.setInsertionPointToEnd(other.getBodyBlock());b.create<InstanceOp>(loc,inner,"used");}
  if(mode==25)make(profile("GGFASEDReadAdmissionWrapper"),{});
  if(mode==26)c.setName("Wrong");
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto engine=b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDTokenEngine"),
      ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
  engine->setAttr("goldengate.bridgeConstructor",b.getDictionaryAttr({
      b.getNamedAttr("axi4Edge",b.getDictionaryAttr({
        b.getNamedAttr("maxFlight",b.getI64IntegerAttr(maxFlight))}))}));
  if(mode!=1)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({
      b.getStringAttr(profile("~GGFASEDWriteAdmissionWrapper|GGFASEDWriteAdmissionWrapper>fased_timing_requests.ar.ready")),
      b.getStringAttr(profile("~GGFASEDWriteAdmissionWrapper|GGFASEDWriteAdmissionWrapper>echo")),
      b.getStringAttr(profile("~GGFASEDWriteAdmissionWrapper|GGFASEDTimingCycleWrapper/sim:GGFASEDResponseReleaserWrapper>echo"))}))})}));
  return root;
}
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
  auto oldTop=named(c,names[0]);auto oldPorts=oldTop.getPorts();std::string error;
  require(succeeded(goldengate::bindFASEDReadAdmission(c,error)),error);
  require(succeeded(verify(*root)),"invalid retirement IR");
  auto top=named(c,c.getName());require(top.getNumPorts()==oldPorts.size()+2,"wrong external ports");
  auto sim=*top.getOps<InstanceOp>().begin();auto it=top.getOps<InstanceOp>().begin();++it;auto counter=*it;
  require(driver(top,counter.getResult(4))==sim.getResult(sim.getNumResults()-1),"R retirement not bound");
  require(driver(top,counter.getResult(0))==top.getBodyBlock()->getArgument(0),"wrong host clock");
  require(driver(top,counter.getResult(1))==sim.getResult(1),"wrong model reset");
  require(driver(top,counter.getResult(2))==sim.getResult(2),"wrong target fire");
  require(driver(top,counter.getResult(5))==top.getBodyBlock()->getArgument(5),"wrong runtime max");
  for(unsigned j=0;j<4;++j) {
    require(top.getPortName(j)==oldPorts[j].name&&top.getPortType(j)==oldPorts[j].type&&top.getPortDirection(j)==oldPorts[j].direction,"copied port changed");
    require(driver(top,j?top.getBodyBlock()->getArgument(j):sim.getResult(j))==
      (j?sim.getResult(j):top.getBodyBlock()->getArgument(j)),"copied connection changed");
  }
  auto ar=cast<BundleType>(cast<BundleType>(top.getPortType(4)).getElements()[2].type);
  require(!ar.getElements()[0].isFlip,"AR ready still externally driven");
  unsigned drivenReady=0,acceptedValid=0;
  for(auto op:top.getOps<StrictConnectOp>()) {
    if(auto sf=op.getDest().getDefiningOp<SubfieldOp>())
      if(sf.getFieldName()=="ready") {require(op.getSrc()==counter.getResult(6),"wrong AR ready");++drivenReady;}
    if(op.getDest()==counter.getResult(3)) {
      auto sf=op.getSrc().getDefiningOp<SubfieldOp>();
      require(sf&&sf.getFieldName()=="valid","wrong AR valid");++acceptedValid;
    }
  }
  require(drivenReady==2&&acceptedValid==1,"AR bindings absent");
  Value observed;
  for(unsigned j=0;j<8;++j) {
    auto m=named(c,names[j]);auto child=*m.getOps<InstanceOp>().begin();
    auto sentinel=child->getAttrOfType<StringAttr>("test.preserved");
    require(sentinel&&sentinel.getValue()=="identity","instance attribute lost");
    require(driver(m,child.getResult(0))==m.getBodyBlock()->getArgument(0),"existing input use lost");
    for(unsigned i=1;i<4;++i)
      require(driver(m,m.getBodyBlock()->getArgument(i))==child.getResult(i),"existing output use lost");
    require(m.getPortName(m.getNumPorts()-1)=="fased_accepted_r_last_fire","observation absent");
    auto v=driver(m,m.getBodyBlock()->getArgument(m.getNumPorts()-1));
    if(j<7)require(v==child.getResult(child.getNumResults()-1),"observation propagation differs");
    else observed=v;
  }
  auto lastAnd=observed.getDefiningOp<AndPrimOp>();require(bool(lastAnd),"not a last-beat handshake");
  auto andOp=lastAnd.getLhs().getDefiningOp<AndPrimOp>();require(bool(andOp),"not an R handshake");
  auto ready=andOp.getLhs().getDefiningOp<SubfieldOp>(),valid=andOp.getRhs().getDefiningOp<SubfieldOp>();
  require(ready&&valid&&ready.getFieldName()=="ready"&&valid.getFieldName()=="valid"&&ready.getInput()==valid.getInput(),"wrong R fields");
  require(ready.getInput()==(*named(c,names[7]).getOps<InstanceOp>().begin()).getResult(4),"wrong response source");
  auto last=lastAnd.getRhs().getDefiningOp<SubfieldOp>();require(last&&last.getFieldName()=="last","missing last gate");
  auto bits=last.getInput().getDefiningOp<SubfieldOp>();require(bits&&bits.getFieldName()=="bits"&&bits.getInput()==ready.getInput(),"wrong R payload");
  auto helper=named(c,"GGFASEDReadAdmission");unsigned regs=0;
  for(auto reg:helper.getOps<RegResetOp>()) {
    ++regs;require(reg.getResult().getType()==UIntType::get(&ctx,countWidth(),false),"wrong read counter width");
    auto r=reg.getResetSignal().getDefiningOp<AndPrimOp>();
    require(r&&r.getLhs()==helper.getBodyBlock()->getArgument(1)&&r.getRhs()==helper.getBodyBlock()->getArgument(2),"reset not gated by fire");
    auto next=driver(helper,reg.getResult()).getDefiningOp<MuxPrimOp>();
    require(next&&next.getSel()==helper.getBodyBlock()->getArgument(2)&&next.getLow()==reg.getResult(),"state not held on stall");
  }
  require(regs==1,"wrong read counter count");
  auto attrs=c->getAttrOfType<ArrayAttr>("rawAnnotations");auto targets=cast<DictionaryAttr>(attrs[0]).getAs<ArrayAttr>("targets");
  require(cast<StringAttr>(targets[0]).getValue()==profile("~GGFASEDReadAdmissionWrapper|GGFASEDReadAdmissionWrapper>fased_timing_requests.ar.ready"),"AR target not transferred");
  require(cast<StringAttr>(targets[1]).getValue()==profile("~GGFASEDReadAdmissionWrapper|GGFASEDReadAdmissionWrapper>echo"),"copied target not moved");
  require(cast<StringAttr>(targets[2]).getValue()==profile("~GGFASEDReadAdmissionWrapper|GGFASEDTimingCycleWrapper/sim:GGFASEDResponseReleaserWrapper>echo"),"hierarchy target changed");
  llvm::outs()<<"Eight final-R observation hops, pending-read counter/admission, existing users and annotation targets passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=26;++mode) {
    auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::bindFASEDReadAdmission(c,error))&&!error.empty(),"unsupported hierarchy accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
  require(succeeded(goldengate::bindFASEDReadAdmission(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::bindFASEDReadAdmission(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"27 atomic rejection cases passed\n";
}

unsigned port(FModuleOp m, llvm::StringRef name) {
  for (unsigned i=0;i<m.getNumPorts();++i) if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: "+name.str());
}
void exportProduction(MLIRContext &ctx,const char *input,const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production read-admission boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()=="GGFASEDReadAdmissionWrapper","expected production read-admission boundary");
  auto top=named(c,c.getName());auto sim=*top.getOps<InstanceOp>().begin();
  require(sim.getModuleName()==names[0],"wrong production top sim");
  auto helper=named(c,"GGFASEDReadAdmission");InstanceOp original;
  for(auto i:top.getOps<InstanceOp>())if(i.getModuleName()==helper.getName())original=i;
  require(bool(original),"missing production read counter");
  require(driver(top,original.getResult(4))==sim.getResult(port(named(c,names[0]),"fased_accepted_r_last_fire")),"R retirement feedback differs");
  require(driver(top,original.getResult(0))==top.getBodyBlock()->getArgument(port(top,"hostClock"))&&
    driver(top,original.getResult(1))==sim.getResult(port(named(c,names[0]),"fased_model_reset"))&&
    driver(top,original.getResult(2))==sim.getResult(port(named(c,names[0]),"fased_tfire"))&&
    driver(top,original.getResult(5))==top.getBodyBlock()->getArgument(port(top,"fased_read_max_reqs")),"clock/reset/fire/maximum binding differs");
  auto arValid=driver(top,original.getResult(3)).getDefiningOp<SubfieldOp>();
  auto ar=arValid?arValid.getInput().getDefiningOp<SubfieldOp>():SubfieldOp();
  require(arValid&&arValid.getFieldName()=="valid"&&ar&&ar.getFieldName()=="ar"&&
    ar.getInput()==sim.getResult(port(named(c,names[0]),"fased_timing_requests")),"wrong accepted AR source");
  Value observed;
  for(unsigned j=0;j<8;++j) {
    auto m=named(c,names[j]);auto v=driver(m,m.getBodyBlock()->getArgument(port(m,"fased_accepted_r_last_fire")));
    if(j<7) {
      auto child=v.getDefiningOp<InstanceOp>();auto next=named(c,names[j+1]);
      require(child&&child.getInstanceName()=="sim"&&child.getModuleName()==names[j+1]&&
        v==child.getResult(port(next,"fased_accepted_r_last_fire")),"final-R observation hop differs");
    }else observed=v;
  }
  auto final=observed.getDefiningOp<AndPrimOp>();
  require(bool(final),"missing final-R handshake");
  auto handshake=final.getLhs().getDefiningOp<AndPrimOp>();
  auto last=final.getRhs().getDefiningOp<SubfieldOp>();
  require(handshake&&last&&last.getFieldName()=="last","missing final-R last qualification");
  auto ready=handshake.getLhs().getDefiningOp<SubfieldOp>(),valid=handshake.getRhs().getDefiningOp<SubfieldOp>();
  auto bits=last.getInput().getDefiningOp<SubfieldOp>();
  require(ready&&valid&&ready.getFieldName()=="ready"&&valid.getFieldName()=="valid"&&
    ready.getInput()==valid.getInput()&&bits&&bits.getFieldName()=="bits"&&
    bits.getInput()==ready.getInput(),"wrong production R fields");
  auto releaser=ready.getInput().getDefiningOp<InstanceOp>();
  require(releaser&&releaser.getInstanceName()=="releaser"&&releaser.getModuleName()=="GGFASEDResponseReleaser"&&
    ready.getInput()==releaser.getResult(port(named(c,"GGFASEDResponseReleaser"),"r")),"wrong production R source");
  Value outerReady;unsigned readyBindings=0;
  for(auto op:top.getOps<StrictConnectOp>())if(op.getSrc()==original.getResult(6)) {
    auto field=op.getDest().getDefiningOp<SubfieldOp>();
    auto channel=field?field.getInput().getDefiningOp<SubfieldOp>():SubfieldOp();
    require(field&&field.getFieldName()=="ready"&&channel&&channel.getFieldName()=="ar","wrong read-ready destination");
    Value base=channel.getInput();
    require(base==top.getBodyBlock()->getArgument(port(top,"fased_timing_requests"))||
      base==sim.getResult(port(named(c,names[0]),"fased_timing_requests")),"wrong read-ready feedback");
    if(isa<BlockArgument>(base))outerReady=op.getDest();
    ++readyBindings;
  }
  require(readyBindings==2&&outerReady,"accepted and observed AR ready must share the counter output");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDReadAdmissionProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());b.insert(helper->clone());
  SmallVector<PortInfo> ports;
  for(auto p:helper.getPorts())if(p.name.getValue()!="rLastFire")ports.push_back(p);
  auto bit=UIntType::get(&ctx,1,false);
  for(auto name:{"rReady","rValid","rLast"})ports.push_back({b.getStringAttr(name),bit,Direction::In});
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDReadAdmissionProbe"),
    ConventionAttr::get(&ctx,Convention::Internal),ports);probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());
  auto counter=b.create<InstanceOp>(c.getLoc(),helper,"counter");IRMapping map;
  for(unsigned i=0;i<helper.getNumPorts();++i)map.map(original.getResult(i),counter.getResult(i));
  map.map(ready.getResult(),probe.getBodyBlock()->getArgument(port(probe,"rReady")));
  map.map(valid.getResult(),probe.getBodyBlock()->getArgument(port(probe,"rValid")));
  map.map(last.getResult(),probe.getBodyBlock()->getArgument(port(probe,"rLast")));
  b.clone(*handshake,map);Value accepted=b.clone(*final,map)->getResult(0);
  for(unsigned i=0;i<helper.getNumPorts();++i) {
    auto name=helper.getPortName(i);Value dest,src;
    if(helper.getPortDirection(i)==Direction::In) {
      dest=original.getResult(i);src=driver(top,dest);
      map.map(src,name=="rLastFire"?accepted:probe.getBodyBlock()->getArgument(port(probe,name)));
    }else {
      src=original.getResult(i);
      if(name=="arReady")dest=outerReady;
      else for(auto &op:*top.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(1)==src) {
        require(!dest,"multiple pending-read output bindings");dest=op.getOperand(0);
      }
      require(bool(dest),"missing counter output binding");
      map.map(dest,probe.getBodyBlock()->getArgument(port(probe,name)));
    }
    bool copied=false;
    for(auto &op:*top.getBodyBlock())if(isa<ConnectOp,StrictConnectOp>(op)&&op.getOperand(0)==dest&&op.getOperand(1)==src) {
      b.clone(op,map);copied=true;
    }
    require(copied,"production counter connection not cloned");
  }
  require(succeeded(verify(isolated)),"invalid production read-admission probe");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export read-admission probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production final-R handshake, AR feedback and counter bindings after validating eight observation hops\n";
}
}
int main(int argc,char **argv) {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {
    require(argc==1||argc==3,"usage: goldengate-fased-read-admission-test [boundary.mlir probe.mlir]");
    if(argc==3){exportProduction(ctx,argv[1],argv[2]);return 0;}
    const llvm::StringRef selectedNames[]{"GGSelectedPrintFASEDWriteAdmissionWrapper", "GGSelectedPrintFASEDWriteRetirementWrapper",
      "GGSelectedPrintFASEDWritePairingWrapper", "GGSelectedPrintFASEDTimingAWQueueWrapper",
      "GGSelectedPrintFASEDWriteLatencyWrapper", "GGSelectedPrintFASEDReadLatencyWrapper",
      "GGSelectedPrintFASEDTimingCycleWrapper", "GGSelectedPrintFASEDResponseReleaserWrapper"};
    for(unsigned flight : {10u,20u}) { maxFlight=flight; behavior(ctx); rejection(ctx); }
    selectedPrint = true;
    for (unsigned i = 0; i < 8; ++i) names[i] = selectedNames[i];
    for(unsigned flight : {10u,20u}) { maxFlight=flight; behavior(ctx); rejection(ctx); }
  }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
