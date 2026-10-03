// See LICENSE for license details.
#include "goldengate/FASEDWriteAdmission.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDWriteRetirementWrapper\" { firrtl.module @GGFASEDWriteRetirementWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");auto c=*root->getOps<CircuitOp>().begin();
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
  if(mode==13)b.create<FModuleOp>(loc,b.getStringAttr("GGFASEDWriteAdmissionWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});
  if(mode==14){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(loc,top,"used");}
  if(mode==15)c.setName("wrong");
  if(mode!=12)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("targets",b.getArrayAttr({b.getStringAttr("~GGFASEDWriteRetirementWrapper"),
      b.getStringAttr("~GGFASEDWriteRetirementWrapper|GGFASEDWriteRetirementWrapper>fased_timing_requests.aw.ready"),
      b.getStringAttr("~GGFASEDWriteRetirementWrapper|GGFASEDWriteRetirementWrapper>fased_pending_writes.wFull"),
      b.getStringAttr("~GGFASEDWriteRetirementWrapper|GGFASEDWritePairingWrapper>fased_target_b_fire")}))})}));
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
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
  auto inner=named(c,c.getName());auto oldPorts=inner.getPorts();std::string oldIR,error;
  {llvm::raw_string_ostream out(oldIR);inner.print(out);}
  require(succeeded(goldengate::bindFASEDWriteAdmission(c,error)),error);
  require(succeeded(verify(*root)),"invalid write admission IR");
  std::string after;{llvm::raw_string_ostream out(after);inner.print(out);}require(oldIR==after,"inner module changed");
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
  const char *expected[]{"~GGFASEDWriteAdmissionWrapper",
    "~GGFASEDWriteAdmissionWrapper|GGFASEDWriteAdmissionWrapper>fased_timing_requests.aw.ready",
    "~GGFASEDWriteAdmissionWrapper|GGFASEDWriteAdmissionWrapper>fased_pending_writes.wFull",
    "~GGFASEDWriteAdmissionWrapper|GGFASEDWritePairingWrapper>fased_target_b_fire"};
  for(unsigned i=0;i<4;++i)require(cast<StringAttr>(targets[i]).getValue()==expected[i],"annotation target changed incorrectly");
  llvm::outs()<<"Closed AW/W admission, observed readiness, payload/AR passthrough and target transfer passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=15;++mode) {
    auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::bindFASEDWriteAdmission(c,error))&&!error.empty(),"unsupported boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
  require(succeeded(goldengate::bindFASEDWriteAdmission(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::bindFASEDWriteAdmission(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"16 atomic rejection cases passed\n";
}
}
int main() {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try{behavior(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
