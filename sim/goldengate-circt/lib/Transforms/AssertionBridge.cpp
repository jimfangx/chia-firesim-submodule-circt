// See LICENSE for license details.
// Oracle: midas/widgets/AssertBridge.scala. No target assertions become host
// Verilog assertions: every failure travels in its target-clock token stream.
#include "goldengate/AssertionBridge.h"
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
using namespace mlir;
using namespace circt::firrtl;
namespace {
struct Constructor { DictionaryAttr bridge,key; StringAttr asserts,reset; ArrayAttr messages; };
LogicalResult constructors(CircuitOp c,SmallVectorImpl<Constructor>& out,std::string& error) {
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  if(!raw){error="Assertion hosts need retained annotations";return failure();}
  llvm::StringSet<> targets;
  for(auto attr:raw) {
    auto bridge=dyn_cast<DictionaryAttr>(attr);
    if(!bridge||bridge.getAs<StringAttr>("class")!=goldengate::AnnotationClasses::BridgeIO||
       bridge.getAs<StringAttr>("widgetClass")!="midas.widgets.AssertBridgeModule")continue;
    auto key=bridge.getAs<DictionaryAttr>("widgetConstructorKey");
    auto asserts=key?key.getAs<StringAttr>("assertPortName"):StringAttr();
    auto reset=key?key.getAs<StringAttr>("resetPortName"):StringAttr();
    auto messages=key?key.getAs<ArrayAttr>("assertMessages"):ArrayAttr();
    auto target=bridge.getAs<StringAttr>("target");
    if(!key||key.getAs<StringAttr>("class")!="midas.widgets.AssertBridgeParameters"||
       !asserts||asserts.getValue().empty()||!reset||reset.getValue().empty()||asserts==reset||
       !messages||messages.empty()||messages.size()>(1<<20)||!target||!targets.insert(target.getValue()).second) {
      error="Assertion hosts require distinct complete constructors";return failure();
    }
    for(auto message:messages)if(!isa<StringAttr>(message)||cast<StringAttr>(message).getValue().contains('\0')) {
      error="Assertion messages must be strings without embedded NULs";return failure();
    }
    out.push_back({bridge,key,asserts,reset,messages});
  }
  return success();
}
FModuleOp bank(CircuitOp circuit,const Constructor& c,StringRef name) {
  auto *ctx=circuit.getContext();OpBuilder b(ctx);auto loc=circuit.getLoc();
  auto uint=[&](unsigned n){return UIntType::get(ctx,n);};
  auto payload=BundleType::get(ctx,{{c.asserts,false,uint(c.messages.size())},{c.reset,false,uint(1)}});
  auto token=BundleType::get(ctx,{{b.getStringAttr("ready"),true,uint(1)},{b.getStringAttr("valid"),false,uint(1)},{b.getStringAttr("bits"),false,uint(32)}});
  auto lanes=FVectorType::get(token,6);
  auto mcr=BundleType::get(ctx,{{b.getStringAttr("read"),false,lanes},{b.getStringAttr("write"),true,lanes},{b.getStringAttr("wstrb"),true,uint(4)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"),ClockType::get(ctx),Direction::In},
    {b.getStringAttr("hostReset"),uint(1),Direction::In},{b.getStringAttr("hValid"),uint(1),Direction::In},
    {b.getStringAttr("fromHostReady"),uint(1),Direction::In},{b.getStringAttr("hBits"),payload,Direction::In},
    {b.getStringAttr("hReady"),uint(1),Direction::Out},{b.getStringAttr("fromHostValid"),uint(1),Direction::Out},
    {b.getStringAttr("mcr"),mcr,Direction::Out}};
  b.setInsertionPointToEnd(circuit.getBodyBlock());
  auto module=b.create<FModuleOp>(loc,b.getStringAttr(name),ConventionAttr::get(ctx,Convention::Internal),ports);
  module->setAttr("goldengate.assertionParameters",c.key);
  b.setInsertionPointToStart(module.getBodyBlock());
  auto arg=[&](unsigned i){return module.getArgument(i);};
  auto k=[&](unsigned w,uint64_t n)->Value{return b.create<ConstantOp>(loc,uint(w),APInt(w,n));};
  auto field=[&](Value v,StringRef n)->Value{return b.create<SubfieldOp>(loc,v,n);};
  auto and_=[&](Value a,Value d)->Value{return b.create<AndPrimOp>(loc,a,d);};
  auto not_=[&](Value a)->Value{return b.create<NotPrimOp>(loc,a);};
  auto mux=[&](Value p,Value a,Value d)->Value{return b.create<MuxPrimOp>(loc,p,a,d);};
  auto connect=[&](Value d,Value s){b.create<StrictConnectOp>(loc,d,s);};
  auto reg=[&](unsigned w,StringRef n)->Value{return b.create<RegResetOp>(loc,uint(w),arg(0),arg(1),k(w,0),n).getResult();};
  Value zero=k(1,0),one=k(1,1),cycles=reg(64,"cycles"),enable=reg(1,"enable"),resume=reg(1,"resume");
  // Queue(depth=2,flow=false,pipe=false), stores payload before the host sees it.
  Value wp=reg(1,"enq_ptr"),rp=reg(1,"deq_ptr"),fullFlag=reg(1,"maybe_full");
  Value same=b.create<EQPrimOp>(loc,wp,rp),full=and_(same,fullFlag),valid=not_(and_(same,not_(fullFlag)));
  connect(arg(5),not_(full));connect(arg(6),one);
  Value push=and_(arg(2),not_(full));
  SmallVector<Value> queued;
  for(auto element:payload.getElements()) {
    Value slots[2];
    for(unsigned slot=0;slot<2;++slot) {
      slots[slot]=b.create<RegOp>(loc,element.type,arg(0),element.name.getValue().str()+"_"+std::to_string(slot)).getResult();
      connect(slots[slot],mux(and_(push,slot?wp:not_(wp)),field(arg(4),element.name),slots[slot]));
    }
    queued.push_back(mux(rp,slots[1],slots[0]));
  }
  Value asserted=b.create<OrRPrimOp>(loc,queued[0]);
  Value fire=and_(asserted,not_(queued[1]));
  Value stallN=b.create<OrPrimOp>(loc,b.create<OrPrimOp>(loc,not_(fire),resume),not_(enable));
  Value pop=and_(valid,stallN);
  connect(wp,mux(push,not_(wp),wp));connect(rp,mux(pop,not_(rp),rp));
  connect(fullFlag,mux(b.create<XorPrimOp>(loc,push,pop),push,fullFlag));
  connect(cycles,mux(pop,b.create<BitsPrimOp>(loc,b.create<AddPrimOp>(loc,cycles,k(64,1)),63,0),cycles));
  // Chisel PriorityEncoder chooses the least significant asserted bit.
  unsigned idWidth=std::max(1u,llvm::Log2_64_Ceil(c.messages.size()));
  Value id=k(idWidth,0);
  for(unsigned i=c.messages.size();i-->0;)
    id=mux(b.create<BitsPrimOp>(loc,queued[0],i,i),k(idWidth,i),id);
  Value values[]{id,and_(fire,valid),b.create<BitsPrimOp>(loc,cycles,31,0),b.create<BitsPrimOp>(loc,cycles,63,32),resume,enable};
  const char *names[]{"id","fire","cycle_low","cycle_high","resume","enable"};
  SmallVector<Attribute> rows;
  for(unsigned i=0;i<6;++i) {
    auto rd=b.create<SubindexOp>(loc,field(arg(7),"read"),i).getResult();
    auto wr=b.create<SubindexOp>(loc,field(arg(7),"write"),i).getResult();
    bool readable=i!=4,writeable=i>=4;
    connect(field(rd,"valid"),one);connect(field(wr,"ready"),one);
    connect(field(rd,"bits"),readable?Value(b.create<PadPrimOp>(loc,values[i],32)):Value(b.create<InvalidValueOp>(loc,uint(32))));
    if(writeable)connect(values[i],mux(field(wr,"valid"),b.create<BitsPrimOp>(loc,field(wr,"bits"),0,0),i==4?zero:values[i]));
    else b.create<AssertOp>(loc,arg(0),not_(field(wr,"valid")),not_(arg(1)),"Assertion register is read only",ValueRange{},"");
    if(!readable)b.create<AssertOp>(loc,arg(0),not_(field(rd,"ready")),not_(arg(1)),"Assertion register is write only",ValueRange{},"");
    rows.push_back(b.getDictionaryAttr({b.getNamedAttr("name",b.getStringAttr(names[i])),b.getNamedAttr("offset",b.getI64IntegerAttr(i*4)),b.getNamedAttr("readable",b.getBoolAttr(readable)),b.getNamedAttr("writeable",b.getBoolAttr(writeable))}));
  }
  module->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));return module;
}
}
LogicalResult goldengate::materializeAssertionBridgeHosts(CircuitOp circuit,unsigned addressBits,unsigned idBits,SmallVectorImpl<FModuleOp>& hosts,std::string& error) {
  if((addressBits!=25&&addressBits!=32)||idBits!=12){error="Assertion bridge requires 25/32-bit ID12 platform control";return failure();}
  SmallVector<Constructor> cs;if(failed(constructors(circuit,cs,error)))return failure();
  if(cs.empty())return success();
  SmallVector<std::string> bases;
  for(auto [i,c]:llvm::enumerate(cs)) {
    std::string base="GGAssertionBridge"+(i?"_"+std::to_string(i):"");
    for(auto suffix:{"Bank","MCRFile","Host"})for(auto m:circuit.getOps<FModuleLike>())if(m.getModuleName()==base+suffix){error="Assertion host module already exists";return failure();}
    bases.push_back(base);
  }
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));SmallVector<std::string> names;
  for(auto [i,c]:llvm::enumerate(cs)) {
    auto registers=bank(*staged,c,bases[i]+"Bank");FModuleOp host;
    if(failed(materializeAutoCounterBridgeAXI(*staged,registers,addressBits,idBits,bases[i]+"Host",bases[i]+"MCRFile",host,error)))return failure();
    OpBuilder b(circuit.getContext());host->setAttr("goldengate.assertionHost",b.getDictionaryAttr({b.getNamedAttr("bridgeTarget",c.bridge.getAs<StringAttr>("target")),b.getNamedAttr("parameters",c.key),b.getNamedAttr("bankModule",registers.getNameAttr()),b.getNamedAttr("mcrModule",b.getStringAttr(bases[i]+"MCRFile")),b.getNamedAttr("addressBits",b.getI64IntegerAttr(addressBits)),b.getNamedAttr("idBits",b.getI64IntegerAttr(idBits)),b.getNamedAttr("queueDepth",b.getI64IntegerAttr(2))}));names.push_back(host.getName().str());
  }
  if(failed(verify(*staged))){error="Assertion materialization produced invalid FIRRTL";return failure();}
  llvm::StringSet<> original;for(auto m:circuit.getOps<FModuleLike>())original.insert(m.getModuleName());
  for(auto &op:llvm::make_early_inc_range(staged->getBodyBlock()->getOperations()))if(auto m=dyn_cast<FModuleLike>(&op))if(!original.count(m.getModuleName()))op.moveBefore(circuit.getBodyBlock(),circuit.getBodyBlock()->end());
  for(auto &name:names)for(auto m:circuit.getOps<FModuleOp>())if(m.getName()==name)hosts.push_back(m);
  return success();
}
