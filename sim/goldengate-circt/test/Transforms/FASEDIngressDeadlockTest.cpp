// See LICENSE for license details.
#include "goldengate/FASEDIngressDeadlock.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode=0) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDIngressIssueWrapper\" { firrtl.module @GGFASEDIngressIssueWrapper() {} } }",&ctx);
  require(bool(root),"fixture parse");
  auto c=*root->getOps<CircuitOp>().begin();(*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(),c.getBodyBlock()->begin());
  auto bit=UIntType::get(&ctx,1,false); auto clock=ClockType::get(&ctx);
  auto token=BundleType::get(&ctx,{{b.getStringAttr("ready"),true,bit},
      {b.getStringAttr("valid"),false,bit},{b.getStringAttr("bits"),false,bit}});
  auto credits=BundleType::get(&ctx,{{b.getStringAttr("awValue"),false,UIntType::get(&ctx,4,false)},
      {b.getStringAttr("wValue"),false,UIntType::get(&ctx,4,false)},
      {b.getStringAttr("awEmpty"),false,bit},{b.getStringAttr("wEmpty"),false,bit},
      {b.getStringAttr("writeReqDone"),false,bit}});
  auto make=[&](llvm::StringRef name,ArrayRef<PortInfo> ports) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    return b.create<FModuleOp>(c.getLoc(),b.getStringAttr(name),ConventionAttr::get(&ctx,Convention::Internal),ports);
  };
  SmallVector<PortInfo> base{{b.getStringAttr("clock"),clock,Direction::In},
      {b.getStringAttr("reset"),bit,Direction::In}};
  auto qp=base;qp.push_back({b.getStringAttr("enq"),token,Direction::In});
  auto queue=make("GGFASEDIngressAWQueue10",qp);
  auto gp=base;gp.push_back({b.getStringAttr(mode==5?"missing":"w_enq"),token,Direction::Out});
  if(mode==12)gp[0].type=bit;
  if(mode==13)gp[1].direction=Direction::Out;
  if(mode==14)gp[2].type=bit;
  auto gates=make("GGFASEDIngressAW",gp);b.setInsertionPointToStart(gates.getBodyBlock());
  b.create<InstanceOp>(c.getLoc(),queue,mode==6?"missing":"awQueue");
  if(mode==11)b.create<InstanceOp>(c.getLoc(),queue,"ingressDeadlock");
  FModuleOp inner=gates;
  for (auto name : {"GGFASEDIngressAWWrapper","GGFASEDIngressWQueueWrapper",
      "GGFASEDIngressARQueueWrapper","GGFASEDIngressCreditsWrapper",
      "GGFASEDIngressOrderWrapper","GGFASEDIngressIssueWrapper"}) {
    auto ports=base;
    if (name==llvm::StringRef("GGFASEDIngressOrderWrapper"))
      ports.push_back({b.getStringAttr(mode==4?"missing":"fased_ingress_order"),token,Direction::Out});
    if (name==llvm::StringRef("GGFASEDIngressIssueWrapper")) {
      ports.push_back({b.getStringAttr(mode==2?"missing":"fased_ingress_relaxed"),bit,Direction::In});
      ports.push_back({b.getStringAttr(mode==3?"missing":"fased_ingress_credits"),credits,Direction::Out});
    }
    if (mode==7 && name==llvm::StringRef("GGFASEDIngressARQueueWrapper"))
      ports.push_back({b.getStringAttr("fased_ingress_deadlock_context"),bit,Direction::In});
    auto parent=make(name,ports);b.setInsertionPointToStart(parent.getBodyBlock());
    b.create<InstanceOp>(c.getLoc(),inner,inner==gates?"ingressAW":mode==8?"wrong":"sim");inner=parent;
  }
  if (mode==9) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),gates,"shared"); }
  if (mode==10) make("GGFASEDIngressDeadlock",{});
  if (mode==15) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),inner,"used"); }
  if (mode!=1)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target",b.getStringAttr("~GGFASEDIngressIssueWrapper|GGFASEDIngressAW>w_enq.valid"))})}));
  return root;
}
struct Eval {
  llvm::DenseMap<Value,bool> values;
  bool get(Value v) {
    if (values.count(v))return values.lookup(v);
    auto *op=v.getDefiningOp();require(op,"missing input");
    if (isa<NotPrimOp>(op))return !get(op->getOperand(0));
    if (isa<AndPrimOp>(op))return get(op->getOperand(0))&&get(op->getOperand(1));
    if (isa<MuxPrimOp>(op))return get(op->getOperand(get(op->getOperand(0))?1:2));
    throw std::runtime_error("unsupported predicate");
  }
};
void behavior(MLIRContext &ctx) {
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();
  auto raw=c->getAttr("rawAnnotations");auto top=named(c,c.getName());auto oldPorts=top.getPorts();
  std::string error;require(succeeded(goldengate::addFASEDIngressDeadlock(c,error)),error);
  require(succeeded(verify(*root)),"invalid deadlock IR");
  require(raw==c->getAttr("rawAnnotations"),"annotations changed");
  require(oldPorts.size()==top.getNumPorts(),"outer ports changed");
  for (auto [j,p]:llvm::enumerate(oldPorts))require(p.name==top.getPortName(j)&&p.type==top.getPortType(j),"outer identity changed");
  auto helper=named(c,"GGFASEDIngressDeadlock");SmallVector<AssertOp> assertions(helper.getOps<AssertOp>());
  require(assertions.size()==2,"missing deadlock assertions");
  require(assertions[0].getMessage().contains("requests w enqueue")&&
      assertions[1].getMessage().contains("requests aw enqueue"),"wrong assertion messages");
  unsigned failures[2]{};
  for (unsigned flags=0;flags<512;++flags) {
    Eval e;auto f=[&](unsigned j){return bool(flags>>j&1);};
    for (unsigned j=1;j<10;++j)e.values[helper.getBodyBlock()->getArgument(j)]=f(j-1);
    bool expected[]{!f(0)&&f(7)&&!f(8)&&(f(1)?f(3):!f(4)),
                    !f(0)&&f(5)&&!f(6)&&(f(1)?f(2):!f(4))};
    for (unsigned j=0;j<2;++j) {
      require(assertions[j].getClock()==helper.getBodyBlock()->getArgument(0),"not host clocked");
      bool failed=e.get(assertions[j].getEnable())&&!e.get(assertions[j].getPredicate());
      require(failed==expected[j],"deadlock predicate/reset enable differs");failures[j]+=failed;
    }
  }
  require(failures[0]==32&&failures[1]==32,"failure cases not covered");
  unsigned contexts=0;
  for(auto m:c.getOps<FModuleOp>())if(m!=top&&m!=helper&&m.getName()!="GGFASEDIngressAWQueue10") {
    require(m.getPortName(m.getNumPorts()-1)=="fased_ingress_deadlock_context","missing policy propagation");++contexts;
    for(auto i:m.getOps<InstanceOp>())if(i.getInstanceName()=="sim"||i.getInstanceName()=="ingressAW")
      require(i.getPortNameStr(i.getNumResults()-1)=="fased_ingress_deadlock_context","stale instance");
  }
  require(contexts==6,"wrong wrapper chain");
  llvm::outs()<<"512 deadlock cases (32 W and 32 AW failures), six internal policy ports passed\n";
}
void rejection(MLIRContext &ctx) {
  for(unsigned mode=1;mode<=15;++mode) {
    auto root=fixture(ctx,mode);auto c=*root->getOps<CircuitOp>().begin();std::string before,after,error;
    {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::addFASEDIngressDeadlock(c,error))&&!error.empty(),"invalid ingress accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
  auto root=fixture(ctx);auto c=*root->getOps<CircuitOp>().begin();std::string error,before,after;
  require(succeeded(goldengate::addFASEDIngressDeadlock(c,error)),error);
  {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::addFASEDIngressDeadlock(c,error)),"repeat accepted");
  {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"repeat mutated IR");
  llvm::outs()<<"16 atomic rejection cases passed\n";
}
}
int main() {
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {behavior(ctx);rejection(ctx);}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
  return 0;
}
