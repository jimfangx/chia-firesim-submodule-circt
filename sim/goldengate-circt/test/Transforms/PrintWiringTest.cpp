// See LICENSE for license details.
#include "goldengate/PrintWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/APSInt.h"
#include <map>
#include <set>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) { std::string text; llvm::raw_string_ostream out(text); op->print(out); return text; }
FModuleOp named(CircuitOp c, StringRef name) {
  for (auto m:c.getOps<FModuleOp>()) if(m.getName()==name) return m;
  return {};
}
Value driver(FModuleOp module, Value port) {
  Value result;
  for (auto connect:module.getBodyBlock()->getOps<StrictConnectOp>())
    if(connect.getDest()==port) { require(!result,"multiple bundle drivers"); result=connect.getSrc(); }
  require(bool(result),"bundle driver missing"); return result;
}
void check(CircuitOp c, ArrayRef<goldengate::PrintStub> stubs,
           ArrayRef<goldengate::WiredPrint> routes, ArrayAttr annotations) {
  require(c->getAttr("rawAnnotations")==annotations,"pending annotations changed");
  auto top=named(c,c.getName());
  std::set<std::string> paths;
  for(auto &route:routes) {
    auto bundle=stubs[route.stubIndex].bundle;
    FModuleOp module=top;
    Value port=route.topPort;
    std::string absolute="~"+c.getName().str()+"|"+top.getName().str();
    for(auto expected:route.instancePath) {
      auto src=dyn_cast<OpResult>(driver(module,port));
      require(bool(src),"hierarchy route does not use native instance result");
      auto instance=dyn_cast<InstanceOp>(src.getOwner());
      require(instance && instance==expected && instance->getParentOfType<FModuleOp>()==module,
              "route instance identity mismatch");
      absolute+="/"+instance.getName().str()+":"+instance.getModuleName().str();
      module=named(c,instance.getModuleName());
      require(bool(module),"route child module missing");
      port=module.getBodyBlock()->getArgument(src.getResultNumber());
      require(port.getType()==bundle.getResult().getType(),"export bundle type changed");
    }
    require(module==bundle->getParentOfType<FModuleOp>() && driver(module,port)==bundle.getResult(),
            "route does not terminate at selected native bundle");
    absolute+=">"+bundle.getName().str();
    auto arg=cast<BlockArgument>(route.topPort);
    require(route.absoluteSource==absolute && paths.insert(absolute).second &&
        route.topTarget=="~"+c.getName().str()+"|"+top.getName().str()+">"+top.getPortName(arg.getArgNumber()).str(),
        "expanded source/top target mismatch");
  }
}
void run(MLIRContext &context, bool complete = false) {
  auto root=parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>,
        in %a: !firrtl.uint<8>, in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>, out %old: !firrtl.uint<8>) {}
      firrtl.module @A(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>,
        in %a: !firrtl.uint<8>, in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>, out %old: !firrtl.uint<8>) {}
      firrtl.module @B(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>,
        in %a: !firrtl.uint<8>, in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>, out %old: !firrtl.uint<8>) {}
      firrtl.module @Leaf(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>,
        in %a: !firrtl.uint<8>, in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>, out %old: !firrtl.uint<8>) {}
      firrtl.module @Unused(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>,
        in %a: !firrtl.uint<8>, in %b: !firrtl.sint<9>, in %wide: !firrtl.uint<129>, out %old: !firrtl.uint<8>) {}
      firrtl.module @Dead(in %clock: !firrtl.clock) {}
    }
  })mlir",&context);
  require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin(); auto top=named(c,"Top");auto leaf=named(c,"Leaf");
  OpBuilder b(&context); SmallVector<Attribute> annos;
  auto print=[&](FModuleOp m,StringRef name,bool args) {
    b.setInsertionPointToEnd(m.getBodyBlock()); auto arg=[&](unsigned i){return m.getBodyBlock()->getArgument(i);};
    b.create<PrintFOp>(m.getLoc(),arg(0),arg(1),"%d %d %x\n\t\r\b\f\"\\ café 😀\x01\x7f",
        args?ValueRange{arg(2),arg(3),arg(4)}:ValueRange{},name);
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::SynthPrintf)),
      b.getNamedAttr("target",b.getStringAttr("~Top|"+m.getName().str()+">"+name.str()))}));
  };
  print(leaf,"message",true); print(leaf,"empty",false); print(top,"local",false);
  auto instance=[&](FModuleOp parent,FModuleOp child,StringRef name) {
    b.setInsertionPointToEnd(parent.getBodyBlock()); auto inst=b.create<InstanceOp>(parent.getLoc(),child,name);
    inst->setAttr("testMetadata",b.getStringAttr("preserved"));
    for(unsigned i=0;i<5;++i) b.create<StrictConnectOp>(parent.getLoc(),inst.getResult(i),parent.getBodyBlock()->getArgument(i));
    return inst;
  };
  instance(named(c,"A"),leaf,"l");instance(named(c,"B"),leaf,"l");instance(named(c,"Unused"),leaf,"l");
  instance(top,named(c,"A"),"left");instance(top,named(c,"B"),"right");auto direct=instance(top,leaf,"direct");
  b.create<StrictConnectOp>(top.getLoc(),top.getBodyBlock()->getArgument(5),direct.getResult(5));
  b.create<WireOp>(top.getLoc(),UIntType::get(&context,1),b.getStringAttr("synthesizedPrintf_left_l_message_wire"));
  b.setInsertionPointToEnd(leaf.getBodyBlock());b.create<StrictConnectOp>(leaf.getLoc(),leaf.getBodyBlock()->getArgument(5),leaf.getBodyBlock()->getArgument(2));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos)); std::string error;
  SmallVector<goldengate::PrintStub> stubs;require(succeeded(goldengate::synthesizePrintStubs(c,stubs,error)),error);
  auto synthesize = [&]() {
    return complete ? goldengate::completePrintSynthesis(c, stubs, error) :
                      goldengate::synthesizePrintChannels(c, stubs, error);
  };
  auto annotations=c->getAttrOfType<ArrayAttr>("rawAnnotations");SmallVector<goldengate::WiredPrint> routes;
  auto before=dump(*root);SmallVector<goldengate::PrintStub> bad(stubs);bad.push_back(stubs[0]);
  require(failed(goldengate::wirePrintStubsToTop(c,bad,routes,error)) && routes.empty() && dump(*root)==before,
          "duplicate source failure mutated circuit");
  b.setInsertionPointToEnd(named(c,"Dead").getBodyBlock());auto dead=b.create<WireOp>(c.getLoc(),stubs[0].bundle.getResult().getType(),b.getStringAttr("dead"));
  bad=stubs;bad.push_back({{},dead,{},"~Top|Dead>dead",{}, {}});before=dump(*root);
  require(failed(goldengate::wirePrintStubsToTop(c,bad,routes,error)) && routes.empty() && dump(*root)==before,
          "unreachable source failure mutated circuit");
  require(succeeded(goldengate::wirePrintStubsToTop(c,stubs,routes,error)),error);
  require(routes.size()==7 && top.getNumPorts()==13 && leaf.getNumPorts()==8 && named(c,"Unused").getNumPorts()==8,
          "shared hierarchy export count mismatch");
  check(c,stubs,routes,annotations);
  SmallVector<goldengate::PrintClockSource> clocks;
  before=dump(*root);
  require(succeeded(goldengate::analyzePrintClockSources(c,stubs,routes,clocks,error)),error);
  require(clocks.size()==7 && dump(*root)==before,"clock analysis changed IR or lost routes");
  for(auto &clock:clocks)
    require(clock.source==top.getBodyBlock()->getArgument(0) && clock.sourceTarget=="~Top|Top>clock",
            "hierarchical clock root mismatch");
  require(succeeded(verify(*root)),"invalid FIRRTL after signature expansion");
  auto oldDriver=cast<OpResult>(driver(top,top.getBodyBlock()->getArgument(5)));
  require(oldDriver.getResultNumber()==5 && cast<InstanceOp>(oldDriver.getOwner()).getName()=="direct",
          "old instance output rewired incorrectly");
  bool collision=false;
  for(auto &r:routes) if(r.absoluteSource=="~Top|Top/left:A/l:Leaf>message_wire")
    collision=r.topTarget!="~Top|Top>synthesizedPrintf_left_l_message_wire";
  require(collision,"top namespace collision lost source identity");
  for(auto m:c.getOps<FModuleOp>()) for(auto i:m.getBodyBlock()->getOps<InstanceOp>())
    require(i->getAttrOfType<StringAttr>("testMetadata") &&
      i->getAttrOfType<StringAttr>("testMetadata").getValue()=="preserved" && i.getPortName(0)=="clock" &&
      i.getPortName(5)=="old", "instance metadata/old port identity changed");
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<WireOp>(top.getLoc(),UIntType::get(&context,1),b.getStringAttr("synthesizedPrintf_clock"));
  before=dump(*root);
  auto invalid=routes;invalid.back().topPort=top.getBodyBlock()->getArgument(0);
  require(failed(goldengate::completePrintClockWiring(c,stubs,invalid,error)) && dump(*root)==before,
          "invalid sink failure mutated clock ports or annotations");
  auto duplicate=routes;duplicate.back().topPort=duplicate.front().topPort;
  require(failed(goldengate::completePrintClockWiring(c,stubs,duplicate,error)) && dump(*root)==before,
          "duplicate sink failure mutated circuit");
  c->setAttr("rawAnnotations",b.getArrayAttr({}));before=dump(*root);
  require(failed(goldengate::completePrintClockWiring(c,stubs,routes,error)) && dump(*root)==before,
          "missing pending annotation failure mutated circuit");
  SmallVector<Attribute> unsupported(annotations.begin(),annotations.end());
  unsupported.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeTopWiring)),
      b.getNamedAttr("target",b.getStringAttr("~Top|Top>unsupported")),
      b.getNamedAttr("clock",b.getStringAttr("~Top|Top>clock"))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(unsupported));before=dump(*root);
  require(failed(goldengate::completePrintClockWiring(c,stubs,routes,error)) && dump(*root)==before,
          "unsupported pending annotation was lost on failure");
  c->setAttr("rawAnnotations",annotations);
  require(succeeded(goldengate::completePrintClockWiring(c,stubs,routes,error)),error);
  require(top.getNumPorts()==14 && top.getPortName(13)=="synthesizedPrintf_clock_0" &&
      top.getPortDirection(13)==Direction::Out && isa<ClockType>(top.getPortType(13)) &&
      driver(top,top.getBodyBlock()->getArgument(13))==top.getBodyBlock()->getArgument(0),
      "shared clock loopback deduplication, namespace or native driver mismatch");
  auto completed=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(completed.size()==10,"output annotations lost preserved SynthPrintf records");
  for(auto [i,route]:llvm::enumerate(routes)) {
    Annotation anno(completed[i]);
    require(anno.isClass(goldengate::AnnotationClasses::BridgeTopWiringOutput) &&
        anno.getMember<StringAttr>("pathlessSource").getValue()==stubs[route.stubIndex].target &&
        anno.getMember<StringAttr>("absoluteSource").getValue()==route.absoluteSource &&
        anno.getMember<StringAttr>("topSink").getValue()==route.topTarget &&
        anno.getMember<StringAttr>("srcClockPort").getValue()=="~Top|Top>clock" &&
        anno.getMember<StringAttr>("sinkClockPort").getValue()=="~Top|Top>synthesizedPrintf_clock_0",
        "five-field BridgeTopWiringOutput annotation mismatch");
  }
  for(auto [i,attr]:llvm::enumerate(annos))
    require(completed[routes.size()+i]==attr,"unconsumed annotation changed");
  require(succeeded(verify(*root)),"invalid FIRRTL after printf clock loopbacks");
  // Reject incomplete contracts and global name conflicts without adding even
  // a reset port. The completed output records remain for bridge construction.
  auto malformed=SmallVector<Attribute>(completed.begin(),completed.end());
  NamedAttrList fields(cast<DictionaryAttr>(malformed.back()));
  fields.set("class",b.getStringAttr(goldengate::AnnotationClasses::BridgeTopWiringOutput));
  malformed.back()=fields.getDictionary(&context);
  c->setAttr("rawAnnotations",b.getArrayAttr(malformed));before=dump(*root);
  require(failed(synthesize()) && dump(*root)==before,
      "malformed completed printf binding partially created channels");
  malformed.assign(completed.begin(),completed.end());
  fields=NamedAttrList(cast<DictionaryAttr>(malformed.front()));
  fields.set("sinkClockPort",b.getStringAttr("~Top|Top>old"));
  malformed.front()=fields.getDictionary(&context);
  c->setAttr("rawAnnotations",b.getArrayAttr(malformed));before=dump(*root);
  require(failed(synthesize()) && dump(*root)==before,
      "non-Clock channel binding partially added reset state");
  malformed.assign(completed.begin(),completed.end());
  malformed.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName",b.getStringAttr(
          top.getPortName(cast<BlockArgument>(routes.front().topPort).getArgNumber()).str()+"_enable"))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(malformed));before=dump(*root);
  require(failed(synthesize()) && dump(*root)==before,
      "printf channel collision failure mutated circuit");
  c->setAttr("rawAnnotations",completed);
  if (complete) {
    auto print = stubs.front().print;
    stubs.front().print = {}; before = dump(*root);
    require(failed(synthesize()) && dump(*root)==before,
        "missing native printf partially created bridge/reset state");
    stubs.front().print = print;
    auto withArgs=llvm::find_if(stubs, [](auto &stub) {
      return !stub.print.getSubstitutions().empty();
    });
    require(withArgs!=stubs.end(),"mixed printf fixture has no operands");
    print=withArgs->print;
    auto arg = print.getSubstitutions()[0];
    auto owner=print->getParentOfType<FModuleOp>();
    print->setOperand(2, owner.getBodyBlock()->getArgument(3)); before = dump(*root);
    require(failed(synthesize()) && dump(*root)==before,
        "mismatched native printf operand partially created bridge/reset state");
    print->setOperand(2, arg);
  }
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<WireOp>(top.getLoc(),UIntType::get(&context,1),
      b.getStringAttr("synthesizedPrintf_clock_0_globalReset"));
  require(succeeded(synthesize()),error);
  require(top.getNumPorts()==15 && top.getPortName(14)=="synthesizedPrintf_clock_0_globalReset_0" &&
      top.getPortDirection(14)==Direction::Out && isa<UIntType>(top.getPortType(14)),
      "printf reset output namespace or direction changed");
  auto zero=driver(top,top.getBodyBlock()->getArgument(14)).getDefiningOp<ConstantOp>();
  require(zero && zero.getValue().isZero(),"printf reset placeholder is not native zero");
  auto channelAnnos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(channelAnnos.size()==(complete?19:28),"printf field/reset channel count mismatch");
  if (!complete) for(auto [i,attr]:llvm::enumerate(completed))
    require(channelAnnos[i]==attr,"pending bridge inputs or unrelated annotations changed");
  require(Annotation(channelAnnos[complete?0:10]).isClass(goldengate::AnnotationClasses::GlobalResetSink),
      "global reset sink was not emitted");
  unsigned channelCount=0;
  std::set<std::string> channelNames;
  for(auto attr:channelAnnos) {
    Annotation anno(attr);
    if(!anno.isClass(goldengate::AnnotationClasses::ChannelConnection))continue;
    auto info=anno.getMember<DictionaryAttr>("channelInfo");
    auto targets=anno.getMember<ArrayAttr>("sources");
    require(info && info.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::PipeChannel &&
        info.getAs<IntegerAttr>("latency").getInt()==0 && !anno.getMember<Attribute>("sinks") &&
        anno.getMember<StringAttr>("clock").getValue()=="~Top|Top>synthesizedPrintf_clock_0" &&
        targets && targets.size()==1 &&
        channelNames.insert(anno.getMember<StringAttr>("globalName").getValue().str()).second,
        "printf source WireChannel schema, clock, or identity mismatch");
    ++channelCount;
  }
  require(channelCount==17 && succeeded(verify(*root)),"printf channel/reset IR invalid");
  if (complete) {
    Annotation bridge(channelAnnos[1]);
    auto key=bridge.getMember<DictionaryAttr>("widgetConstructorKey");
    auto mapping=bridge.getMember<DictionaryAttr>("channelMapping");
    require(bridge.isClass(goldengate::AnnotationClasses::BridgeIO) &&
        bridge.getMember<StringAttr>("target").getValue()=="~Top|Top>synthesizedPrintf" &&
        bridge.getMember<StringAttr>("widgetClass").getValue()==goldengate::AnnotationClasses::PrintBridgeModule &&
        !bridge.getMember<Attribute>("clockInfo") && key && mapping && mapping.size()==17 &&
        key.getAs<StringAttr>("class").getValue()==goldengate::AnnotationClasses::PrintBridgeParameters &&
        key.getAs<StringAttr>("resetPortName").getValue()==top.getPortName(14),
        "printf bridge constructor or mapping schema mismatch");
    for (auto field : mapping)
      require(channelNames.count(field.getName().str()) &&
          cast<StringAttr>(field.getValue()).getValue()==field.getName().getValue(),
          "printf bridge channel mapping is not identity");
    auto ports=key.getAs<ArrayAttr>("printPorts");
    require(ports && ports.size()==routes.size(),"printf bridge lost exported print instances");
    for (auto [i,attr] : llvm::enumerate(ports)) {
      auto port=cast<DictionaryAttr>(attr);
      auto nativeType=cast<BundleType>(routes[i].topPort.getType());
      auto fields=port.getAs<ArrayAttr>("ports");
      require(!port.get("class") &&
          port.getAs<StringAttr>("name").getValue()==top.getPortName(cast<BlockArgument>(routes[i].topPort).getArgNumber()) &&
          port.getAs<StringAttr>("format").getValue()==
              "%d %d %x\\n\\t\\r\\b\\f\\\"\\\\ caf\\u00E9 \\uD83D\\uDE00\\u0001\x7f" &&
          fields.size()==nativeType.getElements().size(),
          "printf bridge instance order or Java format serialization mismatch");
      for (auto [j,field] : llvm::enumerate(nativeType.getElements())) {
        auto tuple=cast<DictionaryAttr>(fields[j]);
        auto type=(isa<UIntType>(field.type)?"UInt<":"SInt<")+
            std::to_string(cast<IntType>(field.type).getWidthOrSentinel())+">";
        require(tuple.size()==1 && tuple.getAs<StringAttr>(field.name.getValue()).getValue()==type,
            "printf field tuple is not SFC singleton-object/type schema");
      }
    }
    for (auto attr : channelAnnos) {
      Annotation anno(attr);
      require(!anno.isClass(goldengate::AnnotationClasses::SynthPrintf) &&
          !anno.isClass(goldengate::AnnotationClasses::BridgeTopWiringOutput),
          "consumed printf annotation survived completed synthesis");
    }
    unsigned prints=0; c.walk([&](PrintFOp){++prints;});
    require(prints==3,"completed synthesis removed original printf operations");
  }
  before=dump(*root);
  require(failed(synthesize()) && dump(*root)==before,
      "repeated printf channel synthesis duplicated state");
  before=dump(*root);SmallVector<goldengate::WiredPrint> empty;
  require(succeeded(goldengate::wirePrintStubsToTop(c,{},empty,error)) && empty.empty() && dump(*root)==before,
          "empty selection mutated circuit");
  require(succeeded(goldengate::completePrintClockWiring(c,{},empty,error)) && dump(*root)==before,
          "empty clock wiring mutated circuit");
}
void runTwoClockWiring(MLIRContext &context, bool complete = false) {
  auto root=parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = [
      {class = "midas.targetutils.SynthPrintfAnnotation", target = "~Top|Leaf>message"}]} {
      firrtl.module @Top(in %clock0: !firrtl.clock, in %clock1: !firrtl.clock,
        in %enable: !firrtl.uint<1>) {}
      firrtl.module @Leaf(in %clock: !firrtl.clock, in %enable: !firrtl.uint<1>) {}
    }
  })mlir",&context);
  require(bool(root),"two clock wiring fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"Top"),leaf=named(c,"Leaf");
  OpBuilder b(&context);b.setInsertionPointToEnd(leaf.getBodyBlock());
  b.create<PrintFOp>(c.getLoc(),leaf.getBodyBlock()->getArgument(0),
      leaf.getBodyBlock()->getArgument(1),"hello\n",ValueRange{},"message");
  b.setInsertionPointToEnd(top.getBodyBlock());
  for(unsigned i=0;i<2;++i) {
    auto inst=b.create<InstanceOp>(c.getLoc(),leaf,i?"right":"left");
    b.create<StrictConnectOp>(c.getLoc(),inst.getResult(0),top.getBodyBlock()->getArgument(i));
    b.create<StrictConnectOp>(c.getLoc(),inst.getResult(1),top.getBodyBlock()->getArgument(2));
  }
  std::string error;SmallVector<goldengate::PrintStub> stubs;SmallVector<goldengate::WiredPrint> routes;
  require(succeeded(goldengate::synthesizePrintStubs(c,stubs,error)),error);
  require(succeeded(goldengate::wirePrintStubsToTop(c,stubs,routes,error)),error);
  // A bad local clock fails before even the first successful root is looped back.
  auto clock=stubs[0].clock;stubs[0].clock={};auto before=dump(*root);
  require(failed(goldengate::completePrintClockWiring(c,stubs,routes,error)) && dump(*root)==before,
      "clock resolution failure mutated circuit");
  stubs[0].clock=clock;
  auto rightClock=routes[1].instancePath.front().getResult(0);
  StrictConnectOp rightConnect;
  for(auto connect:top.getBodyBlock()->getOps<StrictConnectOp>())
    if(connect.getDest()==rightClock)rightConnect=connect;
  require(bool(rightConnect),"second instance clock driver missing");
  auto originalClock=rightConnect.getSrc();rightConnect->setOperand(1,rightClock);before=dump(*root);
  require(failed(goldengate::completePrintClockWiring(c,stubs,routes,error)) && dump(*root)==before,
      "later clock failure committed a partial loopback or annotation rewrite");
  rightConnect->setOperand(1,originalClock);
  require(succeeded(goldengate::completePrintClockWiring(c,stubs,routes,error)),error);
  require(top.getNumPorts()==7,"distinct top clock roots were merged");
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size()==3,"shared printf output expansion count mismatch");
  for(unsigned i=0;i<2;++i) {
    Annotation anno(raw[i]);
    require(driver(top,top.getBodyBlock()->getArgument(5+i))==top.getBodyBlock()->getArgument(i) &&
        anno.getMember<StringAttr>("srcClockPort").getValue()=="~Top|Top>clock"+std::to_string(i) &&
        anno.getMember<StringAttr>("sinkClockPort").getValue()=="~Top|Top>synthesizedPrintf_clock"+std::to_string(i),
        "absolute instance context lost its distinct clock loopback");
  }
  require(succeeded(verify(*root)),"invalid two clock loopback FIRRTL");
  // Reverse output annotations: domain order must still follow clock names.
  c->setAttr("rawAnnotations",b.getArrayAttr({raw[1],raw[0],raw[2]}));
  require(succeeded(complete?goldengate::completePrintSynthesis(c,stubs,error):
      goldengate::synthesizePrintChannels(c,stubs,error)),error);
  require(top.getNumPorts()==9 &&
      top.getPortName(7)=="synthesizedPrintf_clock0_globalReset" &&
      top.getPortName(8)=="synthesizedPrintf_clock1_globalReset",
      "printf domains did not sort by clock identity");
  auto channels=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(channels.size()==(complete?8:9),"two domain channel count mismatch");
  for(unsigned i=0;i<2;++i) {
    auto reset=top.getPortName(7+i).str();
    auto clock="~Top|Top>synthesizedPrintf_clock"+std::to_string(i);
    unsigned offset=complete?i*4:3+i*3;
    Annotation resetAnno(channels[offset]),resetChannel(channels[offset+(complete?2:1)]),
        printChannel(channels[offset+(complete?3:2)]);
    require(resetAnno.getMember<StringAttr>("target").getValue()=="~Top|Top>"+reset &&
        resetChannel.getMember<StringAttr>("clock").getValue()==clock &&
        printChannel.getMember<StringAttr>("clock").getValue()==clock,
        "shared module prints lost per-instance clock domain");
    if (complete) {
      Annotation bridge(channels[offset+1]);
      auto key=bridge.getMember<DictionaryAttr>("widgetConstructorKey");
      auto ports=key.getAs<ArrayAttr>("printPorts");
      require(bridge.isClass(goldengate::AnnotationClasses::BridgeIO) &&
          bridge.getMember<StringAttr>("target").getValue()=="~Top|Top>synthesizedPrintf" &&
          key.getAs<StringAttr>("resetPortName").getValue()==reset && ports.size()==1 &&
          cast<DictionaryAttr>(ports[0]).getAs<StringAttr>("name").getValue()==
              "synthesizedPrintf_"+std::string(i?"right":"left")+"_message_wire" &&
          bridge.getMember<DictionaryAttr>("channelMapping").size()==2,
          "shared printf bridge constructor lost per-clock instance identity");
    }
  }
  require(succeeded(verify(*root)),"two domain printf channel/reset IR invalid");
}
void runClocks(MLIRContext &context) {
  auto root=parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %clock0: !firrtl.clock, in %clock1: !firrtl.clock) {}
      firrtl.module @Leaf(in %clock: !firrtl.clock) {}
      firrtl.module @Forward(in %clock: !firrtl.clock, out %out: !firrtl.clock) {
        %alias = firrtl.node %clock : !firrtl.clock
        firrtl.strictconnect %out, %alias : !firrtl.clock
      }
    }
  })mlir",&context);
  require(bool(root),"clock fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"Top"),leaf=named(c,"Leaf");
  OpBuilder b(&context);b.setInsertionPointToEnd(top.getBodyBlock());
  auto left=b.create<InstanceOp>(c.getLoc(),leaf,"left");
  auto right=b.create<InstanceOp>(c.getLoc(),leaf,"right");
  auto forward=b.create<InstanceOp>(c.getLoc(),named(c,"Forward"),"forward");
  b.create<StrictConnectOp>(c.getLoc(),left.getResult(0),top.getBodyBlock()->getArgument(0));
  b.create<StrictConnectOp>(c.getLoc(),forward.getResult(0),top.getBodyBlock()->getArgument(1));
  auto wire=b.create<WireOp>(c.getLoc(),ClockType::get(&context),b.getStringAttr("alias"));
  b.create<StrictConnectOp>(c.getLoc(),wire.getResult(),forward.getResult(1));
  auto rightConnect=b.create<StrictConnectOp>(c.getLoc(),right.getResult(0),wire.getResult());
  SmallVector<goldengate::PrintStub> stubs{{{}, {}, leaf.getBodyBlock()->getArgument(0), {}, {}, {}}};
  SmallVector<goldengate::WiredPrint> routes{{0,{left},{},"~Top|Top/left:Leaf>data",{}},
      {0,{right},{},"~Top|Top/right:Leaf>data",{}}};
  SmallVector<goldengate::PrintClockSource> sources;std::string error;auto before=dump(*root);
  require(succeeded(goldengate::analyzePrintClockSources(c,stubs,routes,sources,error)),error);
  require(sources.size()==2 && sources[0].sourceTarget=="~Top|Top>clock0" &&
      sources[1].sourceTarget=="~Top|Top>clock1" && dump(*root)==before,
      "shared-module absolute contexts or internal output clock traversal failed");
  auto failure=[&](StringRef diagnostic) {
    sources.clear();before=dump(*root);error.clear();
    require(failed(goldengate::analyzePrintClockSources(c,stubs,routes,sources,error)) &&
        sources.empty() && dump(*root)==before && StringRef(error).contains(diagnostic),
        "clock failure was not atomic: "+error);
  };
  // A second input Clock in the same local cone is ambiguous in the oracle.
  b.setInsertionPoint(rightConnect);
  auto bit0=b.create<AsUIntPrimOp>(c.getLoc(),top.getBodyBlock()->getArgument(0));
  auto bit1=b.create<AsUIntPrimOp>(c.getLoc(),top.getBodyBlock()->getArgument(1));
  auto both=b.create<AndPrimOp>(c.getLoc(),bit0.getResult(),bit1.getResult());
  auto ambiguous=b.create<AsClockPrimOp>(c.getLoc(),both.getResult());
  rightConnect->setOperand(1,ambiguous.getResult());
  require(succeeded(verify(*root)),"ambiguous clock fixture is invalid FIRRTL");
  failure("2 input Clock drivers");
  rightConnect.erase();failure("0 input Clock drivers");
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<StrictConnectOp>(c.getLoc(),right.getResult(0),right.getResult(0));
  failure("combinational cycle");
  SmallVector<goldengate::WiredPrint> empty; sources.clear();before=dump(*root);
  require(succeeded(goldengate::analyzePrintClockSources(c,stubs,empty,sources,error)) &&
      sources.empty() && dump(*root)==before,"empty clock query mutated circuit");
}
void runGolden(MLIRContext &context,StringRef path) {
  auto root=parseSourceFile<ModuleOp>(path,&context);require(bool(root),"golden candidate parse failed");
  auto c=*root->getOps<CircuitOp>().begin();std::string error;SmallVector<goldengate::PrintStub> stubs;
  require(succeeded(goldengate::synthesizePrintStubs(c,stubs,error)),error);
  auto raw=c->getAttrOfType<ArrayAttr>("rawAnnotations");std::map<Operation *,unsigned> original;
  for(auto m:c.getOps<FModuleOp>()) original[m.getOperation()]=m.getNumPorts();
  SmallVector<goldengate::WiredPrint> routes;
  require(succeeded(goldengate::wirePrintStubsToTop(c,stubs,routes,error)),error);
  require(stubs.size()==372 && routes.size()==372,"Rocket expanded count mismatch");
  unsigned ports=0,edges=0;for(auto m:c.getOps<FModuleOp>()) ports+=m.getNumPorts()-original[m.getOperation()];
  for(auto &r:routes) edges+=r.instancePath.size();
  require(ports==2766 && edges==2394,"Rocket golden hierarchy routing mismatch");
  check(c,stubs,routes,raw);require(succeeded(verify(*root)),"invalid Rocket wired FIRRTL");
  llvm::outs()<<"Checked 372 Rocket paths, 2766 bundle ports, 2394 hierarchy edges\n";
}
}
int main(int argc,char **argv) {
  MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {run(context);run(context,true);runClocks(context);runTwoClockWiring(context);runTwoClockWiring(context,true);if(argc==2)runGolden(context,argv[1]);llvm::outs()<<"Print wiring PASS\n";return 0;}
  catch(const std::exception &e){llvm::errs()<<"Print wiring FAIL: "<<e.what()<<'\n';return 1;}
}
