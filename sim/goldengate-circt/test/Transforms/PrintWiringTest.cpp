// See LICENSE for license details.
#include "goldengate/PrintWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
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
void run(MLIRContext &context) {
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
    b.create<PrintFOp>(m.getLoc(),arg(0),arg(1),"%d %d %x\n",args?ValueRange{arg(2),arg(3),arg(4)}:ValueRange{},name);
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
  before=dump(*root);SmallVector<goldengate::WiredPrint> empty;
  require(succeeded(goldengate::wirePrintStubsToTop(c,{},empty,error)) && empty.empty() && dump(*root)==before,
          "empty selection mutated circuit");
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
  try {run(context);runClocks(context);if(argc==2)runGolden(context,argv[1]);llvm::outs()<<"Print wiring PASS\n";return 0;}
  catch(const std::exception &e){llvm::errs()<<"Print wiring FAIL: "<<e.what()<<'\n';return 1;}
}
