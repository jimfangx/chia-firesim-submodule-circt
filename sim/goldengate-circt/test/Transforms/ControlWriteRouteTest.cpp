// See LICENSE for license details.
#include "goldengate/ControlWriteRoute.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <deque>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef s) { if (!ok) throw std::runtime_error(s.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == n) return m;
  throw std::runtime_error("missing module");
}
// Execute the emitted register, memory and expression operations; the deque
// reference in behavior() independently models occupancy and transaction order.
struct Interpreter {
  FModuleOp module; MemOp ram;
  std::vector<uint64_t> memory;
  std::map<std::string,Value> drivers;
  std::map<std::string,uint64_t> memo;
  llvm::DenseMap<Value,uint64_t> state;
  std::string key(Value v) {
    if (auto f=v.getDefiningOp<SubfieldOp>()) return key(f.getInput())+"."+f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m, unsigned count):module(m),memory(count) {
    unsigned pointerWidth=1; while ((1u<<pointerWidth)<count) ++pointerWidth;
    for (auto c:m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()),c.getSrc()).second,"multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(),m.getOps<MemOp>().end())==1,"wrong memory count");
    ram=*m.getOps<MemOp>().begin();
    require(ram.getDepth()==count && ram.getDataType()==UIntType::get(m.getContext(),count,false) && ram.getReadLatency()==0 && ram.getWriteLatency()==1 && ram.getRuw()==RUWAttr::Undefined && ram.getNumResults()==2 && ram.getPortKind(size_t(0))==MemOp::PortKind::Read && ram.getPortKind(size_t(1))==MemOp::PortKind::Write,"wrong RAM geometry");
    unsigned regs=0;
    for (auto r:m.getOps<RegResetOp>()) {
      require(r.getResult().getType()==UIntType::get(m.getContext(),r.getName()=="maybe_full" ? 1 : pointerWidth,false),"wrong pointer width"); ++regs;
    }
    require(regs==(count==1?1u:3u),"wrong register count");
    for (unsigned i=0;i<count;++i) memory[i]=i+1;
  }
  Value arg(unsigned i) {return module.getBodyBlock()->getArgument(i);}
  uint64_t drive(Value v,llvm::StringRef f) {return eval(drivers.at(key(v)+"."+f.str()));}
  uint64_t eval(Value v) {
    auto k=key(v); if(memo.count(k)) return memo.at(k);
    auto *op=v.getDefiningOp(); uint64_t n;
    if(isa_and_nonnull<RegResetOp>(op)) n=state.lookup(v);
    else if(drivers.count(k)) n=eval(drivers.at(k));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op)) n=c.getValue().getZExtValue();
    else if(isa_and_nonnull<AndPrimOp>(op)) n=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if(isa_and_nonnull<XorPrimOp>(op)) n=eval(op->getOperand(0))^eval(op->getOperand(1));
    else if(isa_and_nonnull<NotPrimOp>(op)) n=!eval(op->getOperand(0));
    else if(isa_and_nonnull<EQPrimOp>(op)) n=eval(op->getOperand(0))==eval(op->getOperand(1));
    else if(isa_and_nonnull<AddPrimOp>(op)) n=eval(op->getOperand(0))+eval(op->getOperand(1));
    else if(isa_and_nonnull<MuxPrimOp>(op)) n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
    else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op)) n=(eval(bits.getInput())>>bits.getLo())&((1ULL<<(bits.getHi()-bits.getLo()+1))-1);
    else if(auto f=dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput()==ram.getResult(0) && f.getFieldName()=="data" && drive(ram.getResult(0),"en")==1,"unsupported RAM access");
      n=memory.at(drive(ram.getResult(0),"addr"));
    } else throw std::runtime_error("unsupported operation");
    n&=(1ULL<<*cast<UIntType>(v.getType()).getWidth())-1; memo[k]=n; return n;
  }
  void edge() {
    llvm::DenseMap<Value,uint64_t> next;
    for(auto r:module.getOps<RegResetOp>()) next[r.getResult()]=eval(r.getResetSignal())?eval(r.getResetValue()):eval(drivers.at(key(r.getResult())));
    auto wr=ram.getResult(1);
    if(drive(wr,"en")) { require(drive(wr,"mask")==1,"RAM mask disabled"); memory.at(drive(wr,"addr"))=drive(wr,"data"); }
    state=std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned count=11) {
  std::string source=R"(module {
    firrtl.circuit "GGControlDecodeWrapper" {
      firrtl.module @GGControlDecodeWrapper(in %hostClock: !firrtl.clock,
        in %hostReset: !firrtl.uint<1>, out %ctrl_decode_aw_route: !firrtl.uint<11>, out %other: !firrtl.uint<8>) {}
      firrtl.module @GGControlAddressDecode() {}
    } })";
  source.replace(source.find("uint<11>"),8,"uint<"+std::to_string(count)+">");
  auto root=parseSourceString<ModuleOp>(source,&ctx);
  require(bool(root),"parse failed"); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  SmallVector<Attribute> regions(count,b.getDictionaryAttr({})); named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr(regions));
  SmallVector<Attribute> annos;
  for(auto n:{"hostClock","hostReset","ctrl_decode_aw_route","other"}) annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test.Annotation")),b.getNamedAttr("target",b.getStringAttr("~GGControlDecodeWrapper|GGControlDecodeWrapper>"+std::string(n)))}));
  annos.push_back(b.getDictionaryAttr({b.getNamedAttr("target",b.getStringAttr("~GGControlDecodeWrapper|Model>state"))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &ctx, unsigned count) {
  auto root=fixture(ctx,count); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlWriteRoute(c,error)),error); require(succeeded(verify(*root)),"IR verification failed");
  Interpreter sim(named(c,"GGControlWriteRoute"),count); std::deque<uint64_t> expected; std::mt19937_64 random(177);
  unsigned cycles=0,pushes=0,pops=0,full=0,empty=0,held=0,resets=0,simultaneous=0,zero=0;
  auto cycle=[&](bool reset,bool aw,bool tracker,bool aws,bool w,bool last,bool ws,uint64_t route) {
    sim.memo.clear(); const uint64_t inputs[]{reset,route,aw,tracker,aws,w,last,ws};
    for(unsigned i=1;i<9;++i) sim.memo[sim.key(sim.arg(i))]=inputs[i-1];
    bool qr=expected.size()<count,qv=!expected.empty();
    require(sim.eval(sim.arg(9))==(qr&&tracker&&aws),"AW ready differs");
    require(sim.eval(sim.arg(10))==(aw&&qr&&tracker),"AW slave valid differs");
    require(sim.eval(sim.arg(11))==(aw&&qr&&aws),"AW tracker valid differs");
    require(sim.eval(sim.arg(12))==(qv&&ws) && sim.eval(sim.arg(13))==(w&&qv),"W handshake differs");
    if(qv) {require(sim.eval(sim.arg(14))==expected.front(),"W route reordered");require(sim.eval(sim.arg(15))==(expected.front()==0),"decode-error route differs");zero+=expected.front()==0;}
    bool push=aw&&qr&&tracker&&aws,pop=w&&qv&&ws&&last;
    require(sim.drive(sim.ram.getResult(1),"en")==push,"RAM acceptance differs");
    auto before=sim.memory; unsigned address=sim.drive(sim.ram.getResult(1),"addr");
    if(pop)expected.pop_front();if(push)expected.push_back(route);if(reset)expected.clear();
    sim.edge();
    for(auto r:sim.module.getOps<RegResetOp>())
      if(r.getName()!="maybe_full") require(sim.state.lookup(r.getResult())<count,"pointer failed to wrap");
    for(unsigned i=0;i<count;++i) require(sim.memory[i]==(push&&i==address?route:before[i]),"RAM write/reset semantics differ");
    ++cycles;pushes+=push;pops+=pop;full+=!qr&&aw&&w&&last;empty+=!qv&&w;held+=qv&&w&&ws&&!last;resets+=reset&&push;simultaneous+=push&&pop;
  };
  // Fill, reject replacement on full+pop, then hold each route over W beats.
  for(unsigned repeat=0;repeat<100;++repeat) {
    cycle(true,false,true,true,true,true,true,0);
    cycle(false,false,true,true,true,true,true,0);
    for(unsigned i=0;i<count;++i)cycle(false,true,true,true,false,false,true, i==0?0:1ULL<<(i-1));
    cycle(false,false,true,true,true,false,true,0);
    cycle(false,true,true,true,true,true,true,1ULL<<(count-1));
    for(unsigned i=0;i+1<count;++i) {cycle(false,false,true,true,true,false,true,0);cycle(false,false,true,true,true,true,true,0);}
    cycle(false,true,false,true,false,false,true,1);cycle(false,true,true,false,false,false,true,1);
    cycle(true,true,true,true,false,false,true,1);
  }
  const uint64_t routeMask=(1ULL<<count)-1;
  for(unsigned i=0;i<30000;++i) {
    uint64_t route=i%3==0 ? random()&routeMask : 1ULL<<(random()%count);
    if(i%12==0) route=0;
    cycle(random()%97==0,random()&1,random()&1,random()&1,random()&1,random()&1,random()&1,route);
  }
  require(pushes>1000&&pops>1000&&full>=100&&empty>=100&&held>1000&&resets>10&&(count==1?simultaneous==0:simultaneous>100)&&zero>100,"insufficient corner coverage");
  llvm::outs()<<"depth "<<count<<": "<<cycles<<" cycles; pushes "<<pushes<<", pops "<<pops<<", full/empty "<<full<<"/"<<empty<<", held "<<held<<", reset writes "<<resets<<", simultaneous "<<simultaneous<<"\n";
}
void mapping(MLIRContext &ctx, unsigned count) {
  auto root=fixture(ctx,count);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addControlWriteRoute(c,error)),error);
  auto top=named(c,"GGControlWriteRouteWrapper"),helper=named(c,"GGControlWriteRoute");
  require(top.getNumPorts()==17&&helper.getNumPorts()==16,"wrong port count");
  require(helper->getAttrOfType<IntegerAttr>("goldengate.queueDepth").getInt()==count&&!helper->getAttrOfType<BoolAttr>("goldengate.queueFlow").getValue()&&!helper->getAttrOfType<BoolAttr>("goldengate.queuePipe").getValue(),"queue policy differs");
  std::map<std::string,InstanceOp> instances;for(auto i:top.getOps<InstanceOp>())instances.emplace(i.getName().str(),i);
  require(instances.size()==2,"instances lost");auto sim=instances.at("sim"),route=instances.at("controlWriteRoute");
  Interpreter keys(helper,count);std::map<std::string,std::string>wires;
  for(auto conn:top.getOps<StrictConnectOp>())require(wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc())).second,"duplicate connection");
  auto arg=[&](unsigned i){return top.getBodyBlock()->getArgument(i);};
  for(unsigned i=0;i<3;++i)require(wires.at(keys.key(route.getResult(i)))==keys.key(i==2?sim.getResult(2):arg(i)),"decoder/clock wiring differs");
  for(unsigned i=3;i<16;++i) {
    bool input=i<9;require(wires.at(keys.key(input?route.getResult(i):arg(i+1)))==keys.key(input?arg(i+1):route.getResult(i)),"router boundary wiring differs");
  }
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(annos.size()==5,"annotations lost");
  for(auto [i,a]:llvm::enumerate(annos))require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with(i<4?"~GGControlWriteRouteWrapper|GGControlWriteRouteWrapper>":"~GGControlWriteRouteWrapper|Model>"),"identity differs");
}
void rejection(MLIRContext &ctx) {
  for(unsigned bad=0;bad<12;++bad) {
    auto root=fixture(ctx,bad==10?64:11);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGControlDecodeWrapper");OpBuilder b(&ctx);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2||bad==3){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[bad==2?1:3]=b.getStringAttr(bad==2?"wrongReset":"ctrl_write_route_aw_valid");top.setPortNames(names);}
    if(bad==4){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==5||bad==6){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==5?"GGControlWriteRoute":"GGControlWriteRouteWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==7)named(c,"GGControlAddressDecode")->removeAttr("goldengate.controlRegions");
    if(bad==8)named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr({}));
    if(bad==9){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[2]=b.getStringAttr("wrongRoute");top.setPortNames(names);}
    if(bad==11)named(c,"GGControlAddressDecode")->setAttr("goldengate.controlRegions",b.getArrayAttr(SmallVector<Attribute>(13,b.getDictionaryAttr({}))));
    std::string before,after,error;{llvm::raw_string_ostream o(before);root->print(o);}require(failed(goldengate::addControlWriteRoute(c,error)),"invalid boundary accepted");{llvm::raw_string_ostream o(after);root->print(o);}require(before==after,"rejection mutated IR");
  }
  llvm::outs()<<"Mapping and twelve atomic rejection cases passed\n";
}
}
int main(){try{MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();for(unsigned count:{1u,2u,4u,11u,13u,16u,63u}) behavior(ctx,count);for(unsigned count=1;count<=63;++count) mapping(ctx,count);rejection(ctx);return 0;}catch(const std::exception&e){llvm::errs()<<e.what()<<'\n';return 1;}}
