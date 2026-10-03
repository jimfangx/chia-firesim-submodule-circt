// See LICENSE for license details.
#include "goldengate/ControlReadDispatch.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef s){if(!ok)throw std::runtime_error(s.str());}
std::string dump(ModuleOp m){std::string s;llvm::raw_string_ostream o(s);m.print(o);return s;}
FModuleOp named(CircuitOp c,llvm::StringRef n){for(auto m:c.getOps<FModuleOp>())if(m.getName()==n)return m;throw std::runtime_error("module missing");}
constexpr const char *oldTop="GGControlWidgetWriteWrapper",*newTop="GGControlReadDispatchWrapper";
const std::pair<unsigned,const char*> widgets[]{{2,"tracerv_ctrl"},{4,"loadmem_ctrl"},{5,"peekPokeBridge_ctrl"},
  {6,"uartBridge_ctrl"},{7,"clockBridge_ctrl"},{9,"resetBridge_ctrl"},{10,"cpuStream_ctrl"}};
const std::map<std::string,unsigned> fields{{"addr",25},{"len",8},{"size",3},{"burst",2},{"lock",1},
  {"cache",4},{"prot",3},{"qos",4},{"region",4},{"id",12},{"user",1}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0) {
  std::string address="bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>";
  auto token=[](std::string bits){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: "+bits+">";};
  std::string control="!firrtl.bundle<b flip: "+token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>")+", ar: "+token(address)+", r flip: "+token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>")+">";
  std::string text="module { firrtl.circuit \""+std::string(oldTop)+"\" { firrtl.module @"+oldTop+"(in %other: !firrtl.uint<8>";
  for(auto [slave,port]:widgets)text+=", "+std::string(bad==1&&slave==7?"out":"in")+" %"+port+": "+control;
  struct P {const char *n;unsigned w;const char *d;};
  const P ports[]{{"ctrl_decode_ar_route",11,"out"},{"ctrl_decode_ar_target",4,"out"},
    {"ctrl_decode_ar_addr",25,"in"},{"ctrl_error_ar_ready",1,"out"},{"ctrl_error_ar_valid",1,"in"},
    {"ctrl_error_ar_bits_addr",25,"in"},{"ctrl_error_ar_bits_len",8,"in"},{"ctrl_error_ar_bits_id",12,"in"}};
  for(auto p:ports) {
    if(bad==2&&StringRef(p.n)=="ctrl_error_ar_ready")continue;
    text+=", "+std::string(p.d)+" %"+p.n+": !firrtl.uint<"+std::to_string(bad==3&&StringRef(p.n)=="ctrl_decode_ar_target"?3:p.w)+">";
  }
  if(bad==4)text+=", in %ctrl_read_dispatch_master_ar: !firrtl.uint<1>";
  text+=") {} firrtl.module @GGControlAddressDecode() {} } }";
  auto root=parseSourceString<ModuleOp>(text,&ctx);require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  const char *names[]{"BlockDevBridgeModule_0","FASEDMemoryTimingModel_0","TracerVBridgeModule_0","TSIBridgeModule_0","LoadMemWidget_0","PeekPokeBridgeModule_0","UARTBridgeModule_0","ClockBridgeModule_0","SimulationMaster_0","ResetPulseBridgeModule_0","CPUManagedStreamEngine_0"};
  SmallVector<Attribute> rows,binding,annos;
  for(unsigned i=0;i<11;++i)rows.push_back(b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(bad==5&&i==7?"WrongWidget":names[i])),b.getNamedAttr("slave",b.getI32IntegerAttr(i))}));
  named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  for(auto [i,p]:widgets)binding.push_back(b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(names[i])),
    b.getNamedAttr("port",b.getStringAttr(p)),b.getNamedAttr("slave",b.getI32IntegerAttr(bad==6&&i==7?6:i))}));
  if(bad!=7)named(c,oldTop)->setAttr("goldengate.controlWriteBindings",b.getArrayAttr(binding));
  for(auto ref:{"other","ctrl_decode_ar_addr","clockBridge_ctrl","clockBridge_ctrl.ar.bits.addr","clockBridge_ctrl.b.ready","clockBridge_ctrl.r.bits.data","ctrl_error_ar_valid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~"+std::string(oldTop)+"|"+oldTop+">"+ref))}));
  if(bad!=8)c->setAttr("rawAnnotations",b.getArrayAttr(annos));
  if(bad==9)c.setNameAttr(b.getStringAttr("WrongTop"));
  if(bad==10) {
    b.setInsertionPointToEnd(c.getBodyBlock());auto parent=b.create<FModuleOp>(c.getLoc(),b.getStringAttr("Parent"),ConventionAttr::get(&ctx,Convention::Internal),ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(parent.getBodyBlock());b.create<InstanceOp>(c.getLoc(),named(c,oldTop),"used");
  }
  return root;
}
struct Interpreter {
  FModuleOp module;std::map<std::string,Value> drivers;std::map<std::string,uint64_t> memo;
  std::string key(Value v) {
    if(auto f=v.getDefiningOp<SubfieldOp>())return key(f.getInput())+"."+f.getFieldName().str();
    if(auto a=dyn_cast<BlockArgument>(v))return module.getPortName(a.getArgNumber()).str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m):module(m){for(auto x:m.getOps<StrictConnectOp>())require(drivers.emplace(key(x.getDest()),x.getSrc()).second,"multiple drivers");}
  uint64_t eval(Value v) {
    auto k=key(v);if(memo.count(k))return memo.at(k);auto *op=v.getDefiningOp();uint64_t n;
    if(drivers.count(k))n=eval(drivers.at(k));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue().getZExtValue();
    else if(isa_and_nonnull<AndPrimOp>(op))n=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if(isa_and_nonnull<EQPrimOp>(op))n=eval(op->getOperand(0))==eval(op->getOperand(1));
    else if(isa_and_nonnull<MuxPrimOp>(op))n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
    else if(auto bit=dyn_cast_or_null<BitsPrimOp>(op))n=(eval(bit.getInput())>>bit.getLo())&1;
    else throw std::runtime_error("unsupported expression/missing input");
    return memo[k]=n;
  }
  uint64_t get(std::string n){if(memo.count(n))return memo.at(n);return eval(drivers.at(n));}
};
void test(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addControlReadDispatch(c,error)),error);require(succeeded(verify(*root)),"invalid AR dispatch IR");
  auto helper=named(c,"GGControlReadDispatch");Interpreter sim(helper);std::mt19937_64 rng(180);unsigned cases=0;
  auto sample=[&](unsigned route,bool valid,bool capacity,unsigned mask,bool payloadCheck) {
    unsigned target=rng()%16;sim.memo={{"route",route},{"target",target},{"tracker_ready",capacity},{"master_ar.valid",valid}};
    std::map<std::string,uint64_t> payload;
    for(auto [n,w]:fields)sim.memo["master_ar.bits."+n]=payload[n]=rng()&((1ULL<<w)-1);
    bool ready=false;
    for(unsigned i=0;i<12;++i){auto n=i==11?"err_slave_ar":"slave_"+std::to_string(i)+"_ar";sim.memo[n+".ready"]=(mask>>i)&1;if((i==11&&route==0)||(i<11&&(route&(1<<i))))ready=(mask>>i)&1;}
    require(sim.get("master_ar.ready")==bool(capacity&&ready),"master readiness differs from DecoupledHelper");
    require(sim.get("track_valid")==bool(valid&&ready),"tracker enqueue gate differs");
    require(sim.get("track_tag")==payload.at("id")&&sim.get("track_target")==target,"tracker metadata differs");
    require(bool(sim.get("track_valid")&&capacity)==bool(valid&&sim.get("master_ar.ready")),"tracker and request fires differ");
    for(unsigned i=0;i<12;++i) {
      auto n=i==11?"err_slave_ar":"slave_"+std::to_string(i)+"_ar";bool selected=i==11?route==0:bool(route&(1<<i));
      require(sim.get(n+".valid")==bool(valid&&capacity&&selected),"slave request gate differs");
      if(payloadCheck)for(auto [f,v]:payload)require(sim.get(n+".bits."+f)==v,"payload was gated or altered");
    }
    ++cases;
  };
  for(unsigned i=0;i<12;++i)for(unsigned mask=0;mask<4096;++mask)sample(i==11?0:1<<i,mask&1,mask&2,mask,false);
  for(unsigned i=0;i<5000;++i)sample(rng()%2048,rng()&1,rng()&1,rng()%4096,true);
  auto top=named(c,newTop);require(top.getNumPorts()==20,"incorrect remaining read/response boundary");
  for(auto p:top.getPorts())for(auto [i,n]:widgets)if(p.name==n)require(cast<BundleType>(p.type).getNumElements()==2,"AR still external");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");unsigned i=0;
  for(auto a:annos) {
    auto target=cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue();auto suffix=target.drop_front(target.find('>'));
    require(target=="~"+std::string(newTop)+"|"+((i==0||i==4||i==5)?newTop:oldTop)+suffix.str(),"partial bundle target transfer failed");++i;
  }
  auto before=dump(*root);require(failed(goldengate::addControlReadDispatch(c,error)),"repeat accepted");require(dump(*root)==before,"repeat mutated IR");
  for(unsigned bad=1;bad<=10;++bad){auto r=fixture(ctx,bad);auto circuit=*r->getOps<CircuitOp>().begin();auto s=dump(*r);require(failed(goldengate::addControlReadDispatch(circuit,error)),"malformed boundary accepted");require(dump(*r)==s,"rejection mutated IR");}
  llvm::outs()<<"Control AR dispatch: "<<cases<<" backpressure/route cases, seven bindings, targets, and eleven atomic rejections passed\n";
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{test(ctx);return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
