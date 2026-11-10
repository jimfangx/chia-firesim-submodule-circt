// See LICENSE for license details.
#include "NICTokenQueuesFixture.h"
#include "goldengate/NICLoopbackControl.h"
#include "goldengate/NICHeader.h"
#include "goldengate/NICMMIOBank.h"
#include "goldengate/AnnotationClasses.h"
#include "llvm/ADT/BitVector.h"
static FModuleOp named(CircuitOp c,StringRef n) {
  for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;
  throw std::runtime_error("missing module "+n.str());
}
static std::string printed(ModuleOp m){std::string s;llvm::raw_string_ostream o(s);m.print(o);return s;}
// Keep all handshakes; discard only the three payload monitors that would make
// intentionally unspecified SFC pauser fields observable in this fixture.
static OwningOpRef<ModuleOp> input(MLIRContext &ctx,unsigned widgets,bool nested=false) {
  auto root=fixture(ctx,true,1,widgets,true);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,c.getName());
  llvm::BitVector erase(top.getNumPorts());
  for(auto [i,p]:llvm::enumerate(top.getPorts())) {
    auto name=p.name.getValue();bool pauser=false;
    for(unsigned w=0;w<widgets;++w)for(unsigned j=12;j<15;++j)
      pauser|=name=="w"+std::to_string(w)+"_p"+std::to_string(j)+"_bits";
    if(!pauser)continue;
    auto arg=top.getArgument(i);SmallVector<Operation*> connects;
    for(auto *user:arg.getUsers()){require(isa<StrictConnectOp>(user),"pauser fixture driver");connects.push_back(user);}
    for(auto *op:connects)op->erase();erase.set(i);
  }
  top.erasePorts(erase);
  if(nested) {
    OpBuilder b(&ctx);top.setName("NICUnusedPauserModel");b.setInsertionPointToEnd(c.getBodyBlock());
    auto wrapper=b.create<FModuleOp>(c.getLoc(),b.getStringAttr(c.getName()),top.getConventionAttr(),top.getPorts());
    b.setInsertionPointToStart(wrapper.getBodyBlock());auto child=b.create<InstanceOp>(c.getLoc(),top,"model");
    for(auto [i,p]:llvm::enumerate(top.getPorts()))b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In?child.getResult(i):wrapper.getArgument(i),p.direction==Direction::In?wrapper.getArgument(i):child.getResult(i));
  }
  return root;
}
// Reproduce the actual production obstruction: per-instance FAME payload
// passes through pure muxes and held data registers before a dead model input.
static OwningOpRef<ModuleOp> bufferedInput(MLIRContext &ctx,unsigned bad=0) {
  auto root=input(ctx,1);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,c.getName());
  OpBuilder b(&ctx);auto loc=c.getLoc();auto bit=UIntType::get(&ctx,1,false),word=UIntType::get(&ctx,16,false);
  top.insertPorts({{top.getNumPorts(),PortInfo(b.getStringAttr("otherLiveOutput"),word,Direction::Out)}});
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto pipe=b.create<FModuleOp>(loc,b.getStringAttr("SharedPauserPipe16"),top.getConventionAttr(),ArrayRef<PortInfo>{
      {b.getStringAttr("clock"),ClockType::get(&ctx),Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In},{b.getStringAttr("enable"),bit,Direction::In},
      {b.getStringAttr("data"),word,Direction::In},{b.getStringAttr("out"),word,Direction::Out}});
  b.setInsertionPointToStart(pipe.getBodyBlock());
  auto initial=b.create<ConstantOp>(loc,word,APInt(16,0));
  Value state=b.create<RegResetOp>(loc,word,pipe.getArgument(0),pipe.getArgument(1),initial,"data_state").getResult();
  b.create<StrictConnectOp>(loc,state,b.create<MuxPrimOp>(loc,pipe.getArgument(2),pipe.getArgument(3),state));
  b.create<StrictConnectOp>(loc,pipe.getArgument(4),state);
  b.setInsertionPointToEnd(top.getBodyBlock());auto zero=b.create<ConstantOp>(loc,word,APInt(16,0));
  auto one=b.create<ConstantOp>(loc,bit,APInt(1,1));
  Value tainted;
  for(unsigned j=12;j<15;++j) {
    auto instance=b.create<InstanceOp>(loc,pipe,"pauser"+std::to_string(j));
    auto payload=b.create<SubfieldOp>(loc,top.getArgument(2+j),"bits");
    b.create<StrictConnectOp>(loc,instance.getResult(0),top.getArgument(0));
    b.create<StrictConnectOp>(loc,instance.getResult(1),top.getArgument(1));
    b.create<StrictConnectOp>(loc,instance.getResult(2),one);
    b.create<StrictConnectOp>(loc,instance.getResult(3),payload);
    if(j==12)tainted=instance.getResult(4);
  }
  auto live=b.create<InstanceOp>(loc,pipe,"unrelatedLivePipe");
  b.create<StrictConnectOp>(loc,live.getResult(0),top.getArgument(0));
  b.create<StrictConnectOp>(loc,live.getResult(1),top.getArgument(1));
  b.create<StrictConnectOp>(loc,live.getResult(2),one);
  b.create<StrictConnectOp>(loc,live.getResult(3),zero);
  b.create<StrictConnectOp>(loc,top.getArgument(top.getNumPorts()-1),bad==1?tainted:live.getResult(4));
  if(bad>=2) {
    auto dataBit=b.create<BitsPrimOp>(loc,tainted,0,0);
    if(bad==2)b.create<StrictConnectOp>(loc,live.getResult(1),dataBit);
    if(bad==3)b.create<StrictConnectOp>(loc,live.getResult(2),dataBit);
    if(bad==4)b.create<StrictConnectOp>(loc,b.create<SubfieldOp>(loc,top.getArgument(2+12),"ready"),dataBit);
    if(bad==5)b.create<AssertOp>(loc,top.getArgument(0),dataBit,one,"live pauser assertion",ValueRange{},"");
    if(bad==6)b.create<StrictConnectOp>(loc,live.getResult(0),b.create<AsClockPrimOp>(loc,dataBit));
    if(bad==7) {
      SmallVector<Type> types{MemOp::getTypeForPort(2,word,MemOp::PortKind::Write)};
      SmallVector<Attribute> names{b.getStringAttr("write")};
      auto memory=b.create<MemOp>(loc,types,0,1,2,RUWAttr::Undefined,names,"liveMemory");
      for(StringRef field:{"clk","en","addr","mask","data"}) {
        Value source=field=="clk"?Value(top.getArgument(0)):field=="data"?tainted:Value(one);
        b.create<StrictConnectOp>(loc,b.create<SubfieldOp>(loc,memory.getResult(0),field),source);
      }
    }
  }
  return root;
}
int main(int argc,char **argv){try{
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();std::string error;
  for(unsigned widgets:{1,2})for(bool nested:{false,true}) {
    auto root=input(ctx,widgets,nested);auto c=*root->getOps<CircuitOp>().begin();
    if(argc==3&&widgets==2&&!nested){std::error_code ec;llvm::raw_fd_ostream os(argv[2],ec);require(!ec,"input output");root->print(os);os<<'\n';}
    std::string header="sentinel",peer="sentinel";
    require(succeeded(goldengate::compileNICLoopbackControl(c,32,header,peer,error)),error.c_str());
    require(succeeded(verify(*root)),"invalid complete loopback composition");
    auto top=named(c,c.getName());require(c.getName()=="GGNICLoopbackCPUStreamPlatformWrapper","loopback final identity");
    require(top->hasAttr("goldengate.nicLoopbackGlobalControl"),"missing complete-control marker");
    bool ctrl=false,cpu=false;for(auto p:top.getPorts()){
      auto n=p.name.getValue();ctrl|=n=="ctrl";cpu|=n=="cpu_managed_axi4";
      require(!n.starts_with("ctrl_")&&!n.starts_with("cpu_stream_")&&!n.starts_with("nicBridge_"),"unconsumed transport/control monitor");
    }require(ctrl&&cpu,"missing real shared control or buffered F2 CPU port");
    require(header.find("CPUManagedStreamWidget")!=std::string::npos&&header.find("simplenic_loopback_t(simif, 0, args)")!=std::string::npos&&
        (widgets==1||header.find("simplenic_loopback_t(simif, 1, args)")!=std::string::npos),"complete driver composition");
    require(peer.find("SIMPLENICBRIDGEMODULE_0_from_cpu_stream")!=std::string::npos,"missing allocated disabled stream map");
    auto decoder=named(c,"GGControlAddressDecode");auto regions=decoder->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
    require(regions&&regions.size()==widgets+1,"incomplete global allocation");
    auto counts=named(c,"GGCPUStreamCountBank");require(counts->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters").size()==2*widgets,"count allocation dropped disabled stream");
    unsigned invalids=0;for(auto v:named(c,"GGNICLoopbackTokenQueuesWrapper").getOps<InvalidValueOp>())++invalids;
    require(invalids==3*widgets,"pauser semantics changed rather than proved unused");
    for(auto name:{"GGCPUStreamAWQueue2","GGCPUStreamWQueue2","GGCPUStreamRQueue2","GGCPUStreamBQueue2"})require(bool(named(c,name)),"missing independent AXI channel buffering");
    unsigned addresses=0;c.walk([&](InstanceOp i){addresses+=i.getModuleName()=="GGCPUStreamAWQueue2";});require(addresses==2,"AW/AR must have independent queue instances");
    if(argc>=2&&widgets==2&&!nested){std::error_code ec;llvm::raw_fd_ostream os(argv[1],ec);require(!ec,"output");root->print(os);os<<'\n';}
    // Production driver assembly invokes prepareNICHeader after the shared
    // CPU header. It must append the mode-correct constructors while retaining
    // the complete existing platform header and all hardware.
    {
      OpBuilder b(&ctx); SmallVector<Attribute> rows;
      for (auto a : c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
        auto row=cast<DictionaryAttr>(a);
        if (row.getAs<StringAttr>("class")==goldengate::AnnotationClasses::OutputFile &&
            row.getAs<StringAttr>("fileSuffix")==".const.h") {
          NamedAttrList d(row); d.set("body",b.getStringAttr("retained platform header\n"));
          d.set("goldengate.cpuManagedStreamHeader",b.getBoolAttr(true));
          rows.push_back(d.getDictionary(&ctx));
        } else rows.push_back(row);
      }
      c->setAttr("rawAnnotations",b.getArrayAttr(rows));
      require(succeeded(goldengate::prepareNICHeader(c,error)),error.c_str());
      bool found=false;
      for(auto a:c->getAttrOfType<ArrayAttr>("rawAnnotations")) {
        auto row=cast<DictionaryAttr>(a);
        auto suffix=row.getAs<StringAttr>("fileSuffix");
        if(!suffix || suffix!=".const.h")continue;
        auto body=row.getAs<StringAttr>("body").getValue();
        require(body.starts_with("retained platform header\n")&&body.contains("new simplenic_loopback_t(")&&
            !body.contains("new simplenic_t("),"production header lost platform or loopback ABI");
        require(row.getAs<StringAttr>("goldengate.nicHeaderMode")=="compile_time_loopback","production header mode missing");found=true;
      }
      require(found,"production loopback header missing");
      auto before=printed(*root);
      require(failed(goldengate::prepareNICHeader(c,error))&&printed(*root)==before,"repeated production header mutated IR");
    }
    auto before=printed(*root);std::string h="unchanged",p="unchanged";
    require(failed(goldengate::compileNICLoopbackControl(c,32,h,p,error))&&printed(*root)==before&&h=="unchanged"&&p=="unchanged","second insertion failed atomically");
  }
  // This is the production stage, not the NIC-only selected diagnostic:
  // other bridge constructors/annotations survive for later platform stages.
  for (unsigned widgets : {1,2}) {
    auto root=input(ctx,widgets,true); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    auto rows=llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));
    auto other=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),
        b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.UARTBridgeModule")),
        b.getNamedAttr("widgetConstructorKey",b.getDictionaryAttr({}))});
    rows.push_back(other);c->setAttr("rawAnnotations",b.getArrayAttr(rows));
    auto retainedModel=named(c,"NICUnusedPauserModel");
    require(succeeded(goldengate::addNICLoopbackPlatformQueues(c,error)),error.c_str());
    require(named(c,"NICUnusedPauserModel")==retainedModel,"production stage invalidated retained model handles");
    require(succeeded(verify(*root))&&c.getName()=="GGNICCPUStreamQueuesWrapper","production queue stage invalid");
    require(llvm::is_contained(c->getAttrOfType<ArrayAttr>("rawAnnotations"),other),"production stage lost another bridge");
    // The real buffered transport introduces wrappers between token owner and
    // MMIO. A minimal typed passthrough isolates that live-owner resolution.
    auto inner=named(c,c.getName()); b.setInsertionPointToEnd(c.getBodyBlock());
    auto wrapper=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGNICCPUStreamResponseBufferWrapper"),inner.getConventionAttr(),inner.getPorts());
    b.setInsertionPointToStart(wrapper.getBodyBlock());auto instance=b.create<InstanceOp>(c.getLoc(),inner,"platform");
    for(auto [i,p]:llvm::enumerate(inner.getPorts()))b.create<ConnectOp>(c.getLoc(),p.direction==Direction::In?instance.getResult(i):wrapper.getArgument(i),p.direction==Direction::In?wrapper.getArgument(i):instance.getResult(i));
    c.setName(wrapper.getName());
    require(succeeded(goldengate::addNICLoopbackMMIOBank(c,error)),error.c_str());
    require(succeeded(verify(*root)),"production buffered MMIO invalid");
    auto before=printed(*root);
    require(failed(goldengate::addNICLoopbackPlatformQueues(c,error))&&printed(*root)==before,"repeated production stage mutated input");
  }
  {
    auto root=fixture(ctx,true,1,2,true);auto c=*root->getOps<CircuitOp>().begin();auto before=printed(*root);
    require(failed(goldengate::addNICLoopbackPlatformQueues(c,error))&&printed(*root)==before,"live pauser accepted by production stage");
  }
  for(unsigned bad=0;bad<=7;++bad) {
    auto root=bufferedInput(ctx,bad);auto c=*root->getOps<CircuitOp>().begin();auto before=printed(*root);
    require(succeeded(verify(*root)),"buffered pauser fixture invalid");
    auto result=goldengate::validateNICLoopbackPauserInputs(c,error);
    require(bad?failed(result):succeeded(result),"buffered instance-context pauser proof differs");
    require(printed(*root)==before,"pauser reachability proof mutated input");
  }
  for(unsigned bad=0;bad<8;++bad){
    auto root=bad==0?fixture(ctx,true,1,2,true):input(ctx,2);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,c.getName());OpBuilder b(&ctx);
    if(bad==1){auto names=llvm::to_vector(top.getPortNames());names[1]=b.getStringAttr("missingReset");top.setPortNames(names);}
    if(bad==2){auto rows=llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeIO)),b.getNamedAttr("widgetClass",b.getStringAttr("midas.widgets.UARTBridgeModule"))}));c->setAttr("rawAnnotations",b.getArrayAttr(rows));}
    if(bad==3){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGControlAddressDecode"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==4){auto rows=llvm::to_vector(c->getAttrOfType<ArrayAttr>("rawAnnotations"));rows.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::OutputFile)),b.getNamedAttr("fileSuffix",b.getStringAttr(".const.h")),b.getNamedAttr("body",b.getStringAttr("already emitted"))}));c->setAttr("rawAnnotations",b.getArrayAttr(rows));}
    if(bad==5){b.setInsertionPointToEnd(top.getBodyBlock());auto payload=b.create<SubfieldOp>(c.getLoc(),top.getArgument(14),"bits");auto predicate=b.create<BitsPrimOp>(c.getLoc(),payload,0,0);auto one=b.create<ConstantOp>(c.getLoc(),UIntType::get(&ctx,1,false),APInt(1,1));b.create<AssertOp>(c.getLoc(),top.getArgument(0),predicate,one,"live pauser",ValueRange{},"");}
    if(bad==7){b.setInsertionPointToEnd(top.getBodyBlock());auto condition=b.create<SubfieldOp>(c.getLoc(),top.getArgument(14),"valid");auto conditional=b.create<WhenOp>(c.getLoc(),condition,false);auto body=conditional.getThenBodyBuilder();auto payload=body.create<SubfieldOp>(c.getLoc(),top.getArgument(14),"bits");body.create<NotPrimOp>(c.getLoc(),payload);}
    auto before=printed(*root);std::string h="sentinel",p="sentinel";
    require(failed(goldengate::compileNICLoopbackControl(c,bad==6?35:32,h,p,error)),("invalid composition accepted "+std::to_string(bad)).c_str());
    require(before==printed(*root)&&h=="sentinel"&&p=="sentinel","failed composition mutated input/output");
  }
  llvm::outs()<<"PASS one/two-widget nested NIC loopback shared control, buffered CPU F2 ABI, dead pauser proof and eight atomic rejections\n";return 0;
}catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}}
