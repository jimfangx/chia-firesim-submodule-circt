// See LICENSE for license details.
#include "goldengate/FASEDWriteAdmission.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <iterator>
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m:c.getOps<FModuleOp>()) if (m.getName()==name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0, bool selected=false) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDWriteRetirementWrapper\" { firrtl.module @GGFASEDWriteRetirementWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
  if(selected)c.setName("GGSelectedPrintFASEDWriteRetirementWrapper");
  (*c.getOps<FModuleOp>().begin()).erase();OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());auto loc=c.getLoc();
  auto u=[&](unsigned w){return UIntType::get(&ctx,w,false);};auto bit=u(1);
  auto payload=[&](std::initializer_list<std::pair<llvm::StringRef,unsigned>> fields){
    SmallVector<BundleType::BundleElement> es;for(auto [n,w]:fields)es.push_back({b.getStringAttr(n),false,u(w)});return BundleType::get(&ctx,es);};
  auto dec=[&](BundleType bits,bool reverse=true){return BundleType::get(&ctx,{{b.getStringAttr("ready"),reverse,bit},
    {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bits}});};
  auto address=payload({{"user",1},{"id",mode==5?5u:4u},{"region",4},{"qos",4},{"prot",3},{"cache",4},
    {"lock",1},{"burst",2},{"size",3},{"len",8},{"addr",35}});
  auto data=payload({{"user",1},{"strb",8},{"id",4},{"last",1},{"data",mode==6?32u:64u}});
  auto requests=BundleType::get(&ctx,{{b.getStringAttr("aw"),false,dec(address,mode!=7)},
    {b.getStringAttr("w"),false,dec(data,mode!=8)},{b.getStringAttr("ar"),false,dec(address,mode!=9)}});
  auto pending=payload({{"awValue",mode==10?5u:4u},{"wValue",4},{"awFull",mode==11?2u:1u},{"wFull",1}});
  SmallVector<PortInfo> ps{{b.getStringAttr("fased_timing_requests"),requests,Direction::Out},
    {b.getStringAttr("fased_pending_writes"),pending,Direction::Out},
    {b.getStringAttr("hostClock"),ClockType::get(&ctx),Direction::In},
    {b.getStringAttr("fased_tfire"),bit,Direction::Out},{b.getStringAttr("unrelated"),u(8),Direction::In}};
  if(mode==1||mode==2)ps[mode-1].name=b.getStringAttr("missing");
  if(mode==3||mode==4)ps[mode-3].direction=Direction::In;
  auto top=b.create<FModuleOp>(loc,b.getStringAttr(c.getName()),ConventionAttr::get(&ctx,Convention::Internal),ps);
  if(mode==13)b.create<FModuleOp>(loc,b.getStringAttr(selected?"GGSelectedPrintFASEDWriteAdmissionWrapper":"GGFASEDWriteAdmissionWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==14){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  std::string circuitTarget="~"+c.getName().str(), topTarget=circuitTarget+"|"+top.getName().str()+">";
  if(mode==15)c.setName("wrong");
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr(circuitTarget),
      b.getStringAttr(topTarget+"fased_timing_requests.aw.ready"),
      b.getStringAttr(topTarget+"fased_pending_writes.wFull"),
      b.getStringAttr(circuitTarget+"|GGFASEDWritePairingWrapper>fased_target_b_fire")}))})}));
  return root;
}
std::string key(Value v) {
  if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
  if(auto arg=dyn_cast<BlockArgument>(v))return "port"+std::to_string(arg.getArgNumber());
  if(auto inst=v.getDefiningOp<InstanceOp>())return "sim."+inst.getPortNameStr(cast<OpResult>(v).getResultNumber()).str();
  return "op"+std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
}
struct Wiring {
  std::map<std::string,Value> drivers;
  std::map<std::string,std::string> links;
  Wiring(FModuleOp m) {
    auto connect=[&](Value d,Value s){
      std::function<void(Type,std::string,std::string,bool)> expand=[&](Type t,std::string dk,std::string sk,bool flip){
        if(auto bt=dyn_cast<BundleType>(t))for(auto e:bt.getElements())expand(e.type,dk+"."+e.name.str(),sk+"."+e.name.str(),flip!=e.isFlip);
        else require(links.emplace(flip?sk:dk,flip?dk:sk).second,"duplicate aggregate driver");
      };
      if(isa<BundleType>(d.getType()))expand(d.getType(),key(d),key(s),false);
      else require(drivers.emplace(key(d),s).second,"duplicate leaf driver");
    };
    for(auto op:m.getOps<ConnectOp>())connect(op.getDest(),op.getSrc());
    for(auto op:m.getOps<StrictConnectOp>())connect(op.getDest(),op.getSrc());
    for(auto [k,v]:drivers)require(!links.count(k),"duplicate mixed driver");
  }
  unsigned eval(std::string k,std::map<std::string,unsigned> inputs) {
    if(inputs.count(k))return inputs.at(k);
    if(links.count(k))return eval(links.at(k),inputs);
    require(drivers.count(k),"missing driver");auto v=drivers.at(k);
    if(auto n=v.getDefiningOp<NotPrimOp>())return eval(key(n.getInput()),inputs)^1;
    return eval(key(v),inputs);
  }
};
void behavior(MLIRContext &ctx, bool selected) {
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();
  auto inner=named(c,c.getName());auto oldPorts=inner.getPorts();std::string oldIR,error;
  {llvm::raw_string_ostream out(oldIR);inner.print(out);}
  require(succeeded(goldengate::bindFASEDWriteAdmission(c,error)),error);
  require(succeeded(verify(*root)),"invalid write admission IR");
  std::string after;{llvm::raw_string_ostream out(after);inner.print(out);}require(oldIR==after,"inner module changed");
  const std::string newName=c.getName().str();
  auto top=named(c,c.getName());require(top.getNumPorts()==oldPorts.size(),"port count changed");
  for(unsigned i=0;i<oldPorts.size();++i) {
    require(top.getPortName(i)==oldPorts[i].name&&top.getPortDirection(i)==oldPorts[i].direction,"port identity changed");
    if(i)require(top.getPortType(i)==oldPorts[i].type,"unrelated port type changed");
  }
  auto req=cast<BundleType>(top.getPortType(0)),oldReq=cast<BundleType>(oldPorts[0].type);
  for(unsigned i=0;i<3;++i) {
    auto now=cast<BundleType>(req.getElements()[i].type),old=cast<BundleType>(oldReq.getElements()[i].type);
    for(unsigned j=0;j<3;++j) {
      auto a=now.getElements()[j],z=old.getElements()[j];
      require(a.name==z.name&&a.type==z.type&&a.isFlip==(i<2&&j==0?false:z.isFlip),"unexpected request type change");
    }
  }
  Wiring wiring(top);unsigned nots=0;for(auto n:top.getOps<NotPrimOp>())++nots;require(nots==2,"wrong readiness logic");
  for(unsigned aw=0;aw<2;++aw)for(unsigned w=0;w<2;++w) {
    std::map<std::string,unsigned> inputs{{"sim.fased_pending_writes.awFull",aw},{"sim.fased_pending_writes.wFull",w},
      {"port0.ar.ready",aw^w},{"sim.fased_timing_requests.aw.valid",w},{"sim.fased_timing_requests.w.valid",aw},
      {"sim.fased_timing_requests.aw.bits.id",11},{"sim.fased_timing_requests.w.bits.data",0xa5}};
    require(wiring.eval("sim.fased_timing_requests.aw.ready",inputs)==(aw^1),"wrong inner AW ready");
    require(wiring.eval("port0.aw.ready",inputs)==(aw^1),"wrong observed AW ready");
    require(wiring.eval("sim.fased_timing_requests.w.ready",inputs)==(w^1),"wrong inner W ready");
    require(wiring.eval("port0.w.ready",inputs)==(w^1),"wrong observed W ready");
    require(wiring.eval("sim.fased_timing_requests.ar.ready",inputs)==(aw^w),"AR ready changed");
    require(wiring.eval("port0.aw.valid",inputs)==w&&wiring.eval("port0.w.valid",inputs)==aw,"valid masked");
    require(wiring.eval("port0.aw.bits.id",inputs)==11&&wiring.eval("port0.w.bits.data",inputs)==0xa5,"payload changed");
  }
  auto targets=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
  const std::string expected[]{"~"+newName,
    "~"+newName+"|"+newName+">fased_timing_requests.aw.ready",
    "~"+newName+"|"+newName+">fased_pending_writes.wFull",
    "~"+newName+"|GGFASEDWritePairingWrapper>fased_target_b_fire"};
  for(unsigned i=0;i<4;++i)require(cast<StringAttr>(targets[i]).getValue()==expected[i],"annotation target changed incorrectly");
  llvm::outs()<<"Closed AW/W admission, observed readiness, payload/AR passthrough and target transfer passed\n";
}
void rejection(MLIRContext &ctx, bool selected) {
  for(unsigned mode=1;mode<=15;++mode) {
    auto root=fixture(ctx,mode,selected);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::bindFASEDWriteAdmission(c,error))&&!error.empty(),"unsupported boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx,0,selected);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
  require(succeeded(goldengate::bindFASEDWriteAdmission(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::bindFASEDWriteAdmission(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"16 atomic rejection cases passed\n";
}
unsigned port(FModuleOp m, llvm::StringRef name) {
  for(unsigned i=0;i<m.getNumPorts();++i)if(m.getPortName(i)==name)return i;
  throw std::runtime_error("missing production port: "+name.str());
}
// Validate the production full-flag sources and both inner/observed ready
// drivers, then clone the actual NotPrimOps and output connections. The RTL
// differential feeds this cone from the separately cloned production counters.
void exportProduction(MLIRContext &ctx,const char *input,const char *output) {
  auto root=parseSourceFile<ModuleOp>(input,&ctx);
  require(bool(root)&&succeeded(verify(*root)),"invalid production admission boundary");
  auto c=*root->getOps<CircuitOp>().begin();
  require(c.getName()=="GGFASEDWriteAdmissionWrapper","expected admission boundary");
  auto top=named(c,c.getName());auto instances=top.getOps<InstanceOp>();
  require(std::distance(instances.begin(),instances.end())==1,"wrong admission instance count");
  auto sim=*instances.begin();auto inner=named(c,"GGFASEDWriteRetirementWrapper");
  require(sim.getInstanceName()=="sim"&&sim.getModuleName()==inner.getName(),"wrong admission predecessor");
  Wiring wires(top);auto requestPort=port(top,"fased_timing_requests");
  auto requestType=cast<BundleType>(top.getPortType(requestPort));
  auto oldRequestType=cast<BundleType>(inner.getPortType(port(inner,"fased_timing_requests")));
  require(top.getNumPorts()==inner.getNumPorts(),"admission changed port count");
  for(unsigned i=0;i<top.getNumPorts();++i) {
    require(top.getPortName(i)==inner.getPortName(i)&&top.getPortDirection(i)==inner.getPortDirection(i),"admission changed port identity");
    if(i!=requestPort)require(top.getPortType(i)==inner.getPortType(i),"admission changed unrelated type");
  }
  SmallVector<NotPrimOp> operations;SmallVector<Operation *> connections;
  for(unsigned i=0;i<3;++i) {
    auto current=cast<BundleType>(requestType.getElements()[i].type);
    auto previous=cast<BundleType>(oldRequestType.getElements()[i].type);
    for(unsigned j=0;j<3;++j) {
      auto a=current.getElements()[j],z=previous.getElements()[j];
      require(a.name==z.name&&a.type==z.type&&a.isFlip==(i<2&&j==0?false:z.isFlip),"unexpected admission field type");
    }
    if(i==2)continue;
    std::string channel=i==0?"aw":"w",innerKey="sim.fased_timing_requests."+channel+".ready";
    auto notOp=wires.drivers.at(innerKey).getDefiningOp<NotPrimOp>();
    require(bool(notOp)&&key(notOp.getInput())=="sim.fased_pending_writes."+channel+"Full","wrong readiness full flag");
    auto full=notOp.getInput().getDefiningOp<SubfieldOp>();
    require(full&&full.getInput()==sim.getResult(port(inner,"fased_pending_writes")),"wrong pending counter source");
    std::string outerKey="port"+std::to_string(requestPort)+"."+channel+".ready";
    require(wires.drivers.at(outerKey)==notOp.getResult(),"observed ready differs from accepted ready");
    Operation *connection=nullptr;
    for(auto &op:*top.getBodyBlock())if(isa<StrictConnectOp,ConnectOp>(op)&&key(op.getOperand(0))==outerKey) {
      require(!connection,"duplicate observed ready connection");connection=&op;
    }
    require(connection!=nullptr,"missing observed ready connection");
    operations.push_back(notOp);connections.push_back(connection);
  }
  require(std::distance(top.getOps<NotPrimOp>().begin(),top.getOps<NotPrimOp>().end())==2,"extra readiness logic");
  auto isolated=ModuleOp::create(c.getLoc());OpBuilder b(&ctx);
  b.setInsertionPointToEnd(isolated.getBody());
  auto exported=b.create<CircuitOp>(c.getLoc(),b.getStringAttr("GGFASEDWriteAdmissionProbe"));
  b.setInsertionPointToEnd(exported.getBodyBlock());auto bit=UIntType::get(&ctx,1,false);
  SmallVector<PortInfo> ports;
  for(auto n:{"awFull","wFull"})ports.push_back({b.getStringAttr(n),bit,Direction::In});
  for(auto n:{"awReady","wReady"})ports.push_back({b.getStringAttr(n),bit,Direction::Out});
  auto probe=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGFASEDWriteAdmissionProbe"),
    ConventionAttr::get(&ctx,Convention::Internal),ports);probe.setPublic();
  b.setInsertionPointToEnd(probe.getBodyBlock());IRMapping mapping;
  for(unsigned i=0;i<2;++i) {
    mapping.map(operations[i].getInput(),probe.getBodyBlock()->getArgument(i));
    b.clone(*operations[i],mapping);
    mapping.map(connections[i]->getOperand(0),probe.getBodyBlock()->getArgument(i+2));
    b.clone(*connections[i],mapping);
  }
  require(succeeded(verify(isolated)),"invalid admission probe");
  std::error_code ec;llvm::raw_fd_ostream out(output,ec);require(!ec,"cannot export admission probe");
  isolated.print(out);out<<'\n';out.close();isolated.erase();
  llvm::outs()<<"Cloned production independent AW/W full-flag inversions and observed-ready connections\n";
}
}
int main(int argc,char **argv) {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{
    require(argc==1||argc==3,"usage: goldengate-fased-write-admission-test [boundary.mlir probe.mlir]");
    if(argc==3)exportProduction(ctx,argv[1],argv[2]);
    else for(bool selected:{false,true}){behavior(ctx,selected);rejection(ctx,selected);}
  }catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
