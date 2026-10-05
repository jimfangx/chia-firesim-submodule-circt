// See LICENSE for license details.
#include "goldengate/ControlReadDispatch.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <vector>
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
std::vector<std::pair<unsigned,std::string>> boundWidgets(unsigned count,bool empty=false) {
  std::vector<std::pair<unsigned,std::string>> out;
  if(empty)return out;
  if(count==11)for(auto [i,n]:widgets)out.emplace_back(i,n);
  else for(unsigned i=0;i<count;i+=2)out.emplace_back(i,"widget_"+std::to_string(i)+"_ctrl");
  return out;
}
std::string regionName(unsigned count,unsigned i) {
  if(count!=11)return "Widget_"+std::to_string(i);
  const char *names[]{"BlockDevBridgeModule_0","FASEDMemoryTimingModel_0","TracerVBridgeModule_0","TSIBridgeModule_0","LoadMemWidget_0","PeekPokeBridgeModule_0","UARTBridgeModule_0","ClockBridgeModule_0","SimulationMaster_0","ResetPulseBridgeModule_0","CPUManagedStreamEngine_0"};
  return names[i];
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx,unsigned bad=0,unsigned count=11,bool empty=false) {
  unsigned width=llvm::Log2_64_Ceil(count+1);
  std::string address="bundle<addr: uint<25>, len: uint<8>, size: uint<3>, burst: uint<2>, lock: uint<1>, cache: uint<4>, prot: uint<3>, qos: uint<4>, region: uint<4>, id: uint<12>, user: uint<1>>";
  auto token=[](std::string bits){return "bundle<ready flip: uint<1>, valid: uint<1>, bits: "+bits+">";};
  std::string control="!firrtl.bundle<b flip: "+token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>")+", ar: "+token(address)+", r flip: "+token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>")+">";
  auto ws=boundWidgets(count,empty);
  std::string text="module { firrtl.circuit \""+std::string(oldTop)+"\" { firrtl.module @"+oldTop+"(in %other: !firrtl.uint<8>";
  for(auto [slave,port]:ws)text+=", "+std::string(bad==1&&slave==7?"out":"in")+" %"+port+": "+control;
  struct P {const char *n;unsigned w;const char *d;};
  const P ports[]{{"ctrl_decode_ar_route",count,"out"},{"ctrl_decode_ar_target",width,"out"},
    {"ctrl_decode_ar_addr",25,"in"},{"ctrl_error_ar_ready",1,"out"},{"ctrl_error_ar_valid",1,"in"},
    {"ctrl_error_ar_bits_addr",25,"in"},{"ctrl_error_ar_bits_len",8,"in"},{"ctrl_error_ar_bits_id",12,"in"}};
  for(auto p:ports) {
    if(bad==2&&StringRef(p.n)=="ctrl_error_ar_ready")continue;
    unsigned w=p.w;
    if((bad==3&&StringRef(p.n)=="ctrl_decode_ar_target")||(bad==19&&StringRef(p.n)=="ctrl_decode_ar_route"))--w;
    text+=", "+std::string(bad==20&&StringRef(p.n)=="ctrl_decode_ar_route"?"in":p.d)+" %"+p.n+": !firrtl.uint<"+std::to_string(w)+">";
  }
  if(bad==4)text+=", in %ctrl_read_dispatch_master_ar: !firrtl.uint<1>";
  text+=") {} firrtl.module @GGControlAddressDecode() {} } }";
  auto root=parseSourceString<ModuleOp>(text,&ctx);require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&ctx);
  SmallVector<Attribute> rows,binding,annos;
  unsigned catalogCount=bad==11?0:bad==12?64:count;
  for(unsigned i=0;i<catalogCount;++i) {
    NamedAttrList row;
    if(bad!=15||i!=0)row.set("name",b.getStringAttr(bad==5&&i==7?"WrongWidget":bad==13&&i==0?"":bad==14&&i==1?regionName(count,0):regionName(catalogCount,i)));
    if(bad!=16||i!=0)row.set("slave",b.getI32IntegerAttr(bad==17&&i==0?1:i));
    rows.push_back(row.getDictionary(&ctx));
  }
  auto decoder=named(c,"GGControlAddressDecode");
  if(bad!=18)decoder->setAttr("goldengate.controlRegions",b.getArrayAttr(rows));
  for(auto [i,p]:ws) {
    NamedAttrList row;
    if(bad!=26||i!=7)row.set("name",b.getStringAttr(regionName(count,i)));
    if(bad!=27||i!=7)row.set("port",b.getStringAttr(bad==23&&i==7?ws.front().second:bad==24&&i==7?"":bad==25&&i==7?"missing":p));
    if(bad!=28||i!=7)row.set("slave",b.getI32IntegerAttr(bad==6&&i==7?6:bad==21&&i==7?-1:bad==22&&i==7?count:i));
    binding.push_back(row.getDictionary(&ctx));
  }
  if(bad!=7)named(c,oldTop)->setAttr("goldengate.controlWriteBindings",b.getArrayAttr(binding));
  std::vector<std::string> refs{"other","ctrl_decode_ar_addr","ctrl_error_ar_valid"};
  if(!ws.empty())for(auto tail:{"",".ar.bits.addr",".b.ready",".r.bits.data"})refs.push_back(ws.front().second+tail);
  for(auto ref:refs)annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Target")),b.getNamedAttr("target",b.getStringAttr("~"+std::string(oldTop)+"|"+oldTop+">"+ref))}));
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
Value instancePort(InstanceOp i,StringRef n) {
  for(unsigned p=0;p<i.getNumResults();++p)if(i.getPortName(p)==n)return i.getResult(p);
  throw std::runtime_error("missing instance port");
}
bool fieldPath(Value v,Value base,std::initializer_list<const char*> names) {
  std::vector<const char*> path(names);
  for(auto it=path.rbegin();it!=path.rend();++it) {
    auto f=v.getDefiningOp<SubfieldOp>();if(!f||f.getFieldName()!=*it)return false;v=f.getInput();
  }
  return v==base;
}
unsigned test(MLIRContext &ctx,unsigned count,bool empty=false) {
  auto root=fixture(ctx,0,count,empty);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  auto original=named(c,oldTop)->getAttrOfType<ArrayAttr>("goldengate.controlWriteBindings");
  require(succeeded(goldengate::addControlReadDispatch(c,error)),error);require(succeeded(verify(*root)),"invalid AR dispatch IR");
  auto helper=named(c,"GGControlReadDispatch"),top=named(c,newTop);
  unsigned width=llvm::Log2_64_Ceil(count+1);
  require(helper.getNumPorts()==count+8&&top.getNumPorts()==count+9,"incorrect dispatch geometry");
  require(helper->getAttr("goldengate.controlRegions")==named(c,"GGControlAddressDecode")->getAttr("goldengate.controlRegions"),"helper lost catalog");
  require(top->getAttr("goldengate.controlReadBindings")==original,"bindings changed");
  require(helper.getPorts()[0].type==UIntType::get(&ctx,count,false)&&helper.getPorts()[1].type==UIntType::get(&ctx,width,false),"route/target widths differ");
  Interpreter sim(helper);std::mt19937_64 rng(180+count);unsigned cases=0;
  const uint64_t routeMask=(uint64_t(1)<<count)-1;
  auto sample=[&](uint64_t route,bool valid,bool capacity,uint64_t mask,bool payloadCheck) {
    uint64_t target=rng()&((1ULL<<width)-1);sim.memo={{"route",route},{"target",target},{"tracker_ready",capacity},{"master_ar.valid",valid}};
    std::map<std::string,uint64_t> payload;
    for(auto [n,w]:fields)sim.memo["master_ar.bits."+n]=payload[n]=rng()&((1ULL<<w)-1);
    bool ready=false;
    for(unsigned i=0;i<=count;++i){auto n=i==count?"err_slave_ar":"slave_"+std::to_string(i)+"_ar";sim.memo[n+".ready"]=(mask>>i)&1;if((i==count&&route==0)||(i<count&&(route&(1ULL<<i))))ready=(mask>>i)&1;}
    require(sim.get("master_ar.ready")==bool(capacity&&ready),"master readiness differs from DecoupledHelper");
    require(sim.get("track_valid")==bool(valid&&ready),"tracker enqueue gate differs");
    require(sim.get("track_tag")==payload.at("id")&&sim.get("track_target")==target,"tracker metadata differs");
    require(bool(sim.get("track_valid")&&capacity)==bool(valid&&sim.get("master_ar.ready")),"tracker and request fires differ");
    for(unsigned i=0;i<=count;++i) {
      auto n=i==count?"err_slave_ar":"slave_"+std::to_string(i)+"_ar";bool selected=i==count?route==0:bool(route&(1ULL<<i));
      require(sim.get(n+".valid")==bool(valid&&capacity&&selected),"slave request gate differs");
      if(payloadCheck)for(auto [f,v]:payload)require(sim.get(n+".bits."+f)==v,"payload was gated or altered");
    }
    ++cases;
  };
  // Independent valid/capacity combinations, including all slave-ready masks
  // at small geometries. Wide catalogs use walking ready and route bits.
  for(unsigned i=0;i<=count;++i)for(unsigned gates=0;gates<4;++gates) {
    uint64_t route=i==count?0:1ULL<<i;
    if(count<=11)for(unsigned mask=0;mask<(1U<<(count+1));++mask)sample(route,gates&1,gates&2,mask,false);
    else for(unsigned j=0;j<=count;++j)sample(route,gates&1,gates&2,1ULL<<j,false);
    sample(route,gates&1,gates&2,~uint64_t(0),true);
    sample(route,gates&1,gates&2,0,true);
  }
  // Every pair of matching routes checks last-connect priority with opposing
  // ready values, even when a low index is ready and a higher one is stalled.
  for(unsigned i=0;i<count;++i)for(unsigned j=i+1;j<count;++j)
    for(unsigned gates=0;gates<4;++gates)for(auto mask:{1ULL<<i,1ULL<<j})
      sample((1ULL<<i)|(1ULL<<j),gates&1,gates&2,mask,false);
  for(unsigned i=0;i<1000;++i)sample(rng()&routeMask,rng()&1,rng()&1,rng(),true);
  InstanceOp inner,dispatch;
  for(auto i:top.getOps<InstanceOp>())if(i.getModuleName()==oldTop)inner=i;else if(i.getModuleName()=="GGControlReadDispatch")dispatch=i;
  require(bool(inner)&&bool(dispatch),"wrapper instances missing");
  llvm::DenseMap<Value,Value> connections;
  for(auto x:top.getOps<StrictConnectOp>())require(connections.try_emplace(x.getDest(),x.getSrc()).second,"duplicate strict wrapper driver");
  auto source=[&](Value v){return connections.lookup(v);};
  auto outside=[&](StringRef n)->Value {for(auto [i,p]:llvm::enumerate(top.getPorts()))if(p.name.getValue()==n)return top.getBodyBlock()->getArgument(i);throw std::runtime_error("missing wrapper port");};
  require(source(instancePort(dispatch,"route"))==instancePort(inner,"ctrl_decode_ar_route")&&source(instancePort(dispatch,"target"))==instancePort(inner,"ctrl_decode_ar_target"),"decoder identity crossed");
  require(fieldPath(source(instancePort(inner,"ctrl_decode_ar_addr")),outside("ctrl_read_dispatch_master_ar"),{"bits","addr"}),"decoder address not from master");
  auto ws=boundWidgets(count,empty);
  for(auto [i,n]:ws) {
    require(cast<BundleType>(outside(n).getType()).getNumElements()==2,"AR still external");
    unsigned ar=0,responses=0;
    for(auto x:top.getOps<ConnectOp>()) {
      ar+=fieldPath(x.getDest(),instancePort(inner,n),{"ar"})&&x.getSrc()==instancePort(dispatch,"slave_"+std::to_string(i)+"_ar");
      for(auto ch:{"b","r"})responses+=fieldPath(x.getDest(),outside(n),{ch})&&fieldPath(x.getSrc(),instancePort(inner,n),{ch});
    }
    require(ar==1&&responses==2,"mapped AR or remaining b/r crossed sources");
  }
  auto err=instancePort(dispatch,"err_slave_ar");unsigned errorReady=0;
  for(auto [dest,src]:connections)errorReady+=fieldPath(dest,err,{"ready"})&&src==instancePort(inner,"ctrl_error_ar_ready");
  require(errorReady==1&&fieldPath(source(instancePort(inner,"ctrl_error_ar_valid")),err,{"valid"}),"error handshake crossed");
  for(auto n:{"addr","len","id"})require(fieldPath(source(instancePort(inner,"ctrl_error_ar_bits_"+std::string(n))),err,{"bits",n}),"error payload crossed");
  for(unsigned i=0;i<count;++i) {
    bool mapped=false;for(auto [j,n]:ws)mapped|=i==j;if(mapped)continue;
    auto name="slave_"+std::to_string(i)+"_ar";unsigned found=0;
    for(auto x:top.getOps<ConnectOp>())found+=x.getDest()==outside("ctrl_read_dispatch_"+name)&&x.getSrc()==instancePort(dispatch,name);
    require(found==1,"unbound AR not exposed");
  }
  for(auto n:{"master_ar","tracker_ready","track_valid","track_tag","track_target"}) {
    unsigned found=0;auto external=outside("ctrl_read_dispatch_"+std::string(n));auto port=instancePort(dispatch,n);
    bool input=StringRef(n)=="master_ar"||StringRef(n)=="tracker_ready";
    for(auto x:top.getOps<ConnectOp>())found+=x.getDest()==(input?port:external)&&x.getSrc()==(input?external:port);
    require(found==1,"master/tracker boundary crossed");
  }
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");unsigned i=0;
  for(auto a:annos) {
    auto d=cast<DictionaryAttr>(a);auto target=d.getAs<StringAttr>("target").getValue();auto suffix=target.drop_front(target.find('>'));
    require(d.getAs<StringAttr>("class").getValue()=="test.Target","annotation class changed");
    require(target=="~"+std::string(newTop)+"|"+((i==0||i==5||i==6)?newTop:oldTop)+suffix.str(),"partial bundle target transfer failed");++i;
  }
  auto before=dump(*root);require(failed(goldengate::addControlReadDispatch(c,error)),"repeat accepted");require(dump(*root)==before,"repeat mutated IR");
  return cases;
}
}
int main(){MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();try{
  unsigned cases=0;
  for(unsigned count:{1,2,3,11,13,31,63})cases+=test(ctx,count);
  cases+=test(ctx,3,true);
  for(unsigned bad=1;bad<=28;++bad){auto r=fixture(ctx,bad);auto circuit=*r->getOps<CircuitOp>().begin();auto s=dump(*r);std::string error;require(failed(goldengate::addControlReadDispatch(circuit,error)),"malformed boundary accepted");require(!error.empty()&&dump(*r)==s,"rejection mutated IR or omitted diagnostic");}
  llvm::outs()<<"Control AR dispatch: "<<cases<<" independent backpressure/route cases across 1/2/3/11/13/31/63 regions and empty bindings; wrapper wiring, targets and 36 atomic rejections passed\n";
  return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
