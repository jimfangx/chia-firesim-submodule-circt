// See LICENSE for license details.
#include "goldengate/FASEDBankStateTracker.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
std::string print(Operation *op) { std::string text; llvm::raw_string_ostream os(text); op->print(os); return text; }
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value,Value> drivers;
  llvm::DenseMap<Value,APInt> memo, state;
  std::map<std::string,Value> registers;
  std::map<std::string,uint64_t> timings;
  Interpreter(FModuleOp module) : module(module) {
    for (auto c : module.getOps<StrictConnectOp>()) drivers[c.getDest()]=c.getSrc();
    for (auto r : module.getOps<RegResetOp>()) { state[r.getResult()]=APInt(width(r.getResult()),0); registers[r.getName().str()]=r.getResult(); }
    for (auto r : module.getOps<RegOp>()) { state[r.getResult()]=APInt(width(r.getResult()),0); registers[r.getName().str()]=r.getResult(); }
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(unsigned i, uint64_t v) { memo[arg(i)]=APInt(width(arg(i)),v); }
  APInt eval(Value v) {
    if (memo.count(v)) return memo.lookup(v);
    unsigned w=width(v); APInt n(w,0); auto *op=v.getDefiningOp();
    if (state.count(v)) n=state.lookup(v);
    else if (drivers.count(v)) n=eval(drivers.lookup(v));
    else if (auto c=dyn_cast_or_null<ConstantOp>(op)) n=c.getValue();
    else if (auto f=dyn_cast_or_null<SubfieldOp>(op)) { require(f.getInput()==arg(7),"unexpected bundle"); n=APInt(w,timings.at(f.getFieldName().str())); }
    else if (auto bits=dyn_cast_or_null<BitsPrimOp>(op)) n=eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n=eval(op->getOperand(eval(op->getOperand(0)).isZero()?2:1));
    else if (isa_and_nonnull<AndPrimOp>(op)) n=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n=eval(op->getOperand(0))|eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n=~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n=APInt(1,eval(op->getOperand(0))==eval(op->getOperand(1)));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n=APInt(1,eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n=eval(op->getOperand(0)).zextOrTrunc(w)+eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n=eval(op->getOperand(0)).zextOrTrunc(w)-eval(op->getOperand(1)).zextOrTrunc(w);
    else throw std::runtime_error("unsupported FIRRTL operation or undriven value");
    n=n.zextOrTrunc(w); memo[v]=n; return n;
  }
  uint64_t get(Value v) { return eval(v).getZExtValue(); }
  void edge() {
    llvm::DenseMap<Value,APInt> next;
    for(auto r:module.getOps<RegResetOp>()) next[r.getResult()]=get(r.getResetSignal())?eval(r.getResetValue()):eval(drivers.lookup(r.getResult()));
    for(auto r:module.getOps<RegOp>()) next[r.getResult()]=eval(drivers.lookup(r.getResult()));
    state=std::move(next);
  }
};
struct Reference {
  bool active=false; unsigned row=0;
  // PRE, ACT, CAS: the three independent Scala DownCounter instances.
  std::array<unsigned,3> delay{};
  std::array<bool,4> legal() const { return {active&&!delay[2],active&&!delay[2],active&&!delay[0],!active&&!delay[1]}; }
  void step(bool reset,bool fire,unsigned cmd,bool uses,bool autoPRE,unsigned newRow,const std::map<std::string,uint64_t> &t) {
    if(!fire) return;
    auto time=[&](const char *n){return int(t.at(n));};
    std::array<int,3> set{-1,-1,-1};
    if(uses) switch(cmd) {
      case 1: active=true; row=newRow; set={time("tRAS")-1,time("tRC")-1,time("tRCD")-time("tAL")-1}; break;
      case 2: active=false; set[1]=time("tRP")-1; break;
      case 3: if(autoPRE) { active=false; set[1]=time("tCWD")+time("tAL")+time("tWR")+time("tCCD")+time("tRP")+1; }
              else set[0]=time("tCWD")+time("tAL")+time("tWR")+time("tCCD")-1; break;
      case 4: if(autoPRE) { active=false; set[1]=time("tRTP")+time("tAL")+time("tRP")-1; }
              else set[0]=time("tRTP")+time("tAL")-1; break;
    }
    // -1 can also be a real unsigned-underflow set, so command selection must
    // distinguish it from an absent set. This deliberately uses oracle events.
    std::array<bool,3> valid{uses&&(cmd==1||((cmd==3||cmd==4)&&!autoPRE)),
      uses&&(cmd==1||cmd==2||((cmd==3||cmd==4)&&autoPRE)),uses&&cmd==1};
    for(unsigned i=0;i<3;++i) { unsigned value=unsigned(set[i])&127; if(valid[i]&&value>=delay[i])delay[i]=value; else if(delay[i])--delay[i]; }
    if(reset) { active=false; delay.fill(0); } // openRowAddr is not reset.
  }
};
void behavior(MLIRContext &ctx, llvm::StringRef output) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
  auto circuit=*root->getOps<CircuitOp>().begin();
  circuit->setAttr("test.keep",StringAttr::get(&ctx,"identity"));
  auto before=print((*circuit.getOps<FModuleOp>().begin()).getOperation());
  FModuleOp helper; std::string error;
  require(succeeded(goldengate::materializeFASEDBankStateTracker(circuit,helper,error)),error);
  require(succeeded(verify(*root)),"invalid helper IR");
  require(before==print((*circuit.getOps<FModuleOp>().begin()).getOperation())&&circuit.getName()=="Top","changed existing top");
  require(circuit->getAttrOfType<StringAttr>("test.keep").getValue()=="identity","changed circuit metadata");
  Interpreter sim(helper); Reference model; std::mt19937_64 rng(0x44524433);
  std::array<const char*,17> names{"tAL","tCAS","tCMD","tCWD","tCCD","tFAW","tRAS","tREFI","tRC","tRCD","tRFC","tRRD","tRP","tRTP","tRTRS","tWR","tWTR"};
  std::array<unsigned,17> defaults{0,14,1,10,4,25,33,7800,47,14,160,8,14,8,2,15,8};
  for(unsigned i=0;i<names.size();++i)sim.timings[names[i]]=defaults[i];
  require(sim.registers.size()==5,"unexpected bank state");
  auto timings=cast<BundleType>(helper.getPorts()[7].type);
  require(timings.getElements().size()==17&&*cast<UIntType>(helper.getPorts()[6].type).getWidth()==26,"selected organization shape");
  unsigned cases=0,illegal=0,stalledReset=0,underflows=0,smallerSets=0;
  auto sample=[&](bool reset,bool fire,unsigned cmd,bool uses,bool autoPRE,unsigned row) {
    sim.memo.clear();sim.put(1,reset);sim.put(2,fire);sim.put(3,cmd);sim.put(4,uses);sim.put(5,autoPRE);sim.put(6,row);
    auto legal=model.legal();
    for(unsigned i=0;i<4;++i)require(sim.get(sim.arg(8+i))==legal[i],"command legality differs");
    require(sim.get(sim.arg(12))==model.active&&sim.get(sim.arg(13))==model.row,"bank state/row output differs");
    unsigned assertions=0,index=0;
    constexpr unsigned assertionCommands[]{1,2,4,3}, legalIndices[]{3,2,1,0};
    for(auto a:helper.getOps<AssertOp>()) { bool enabled=sim.get(a.getEnable()); assertions+=enabled;
      require(index<4&&enabled==bool(fire&&!reset&&uses&&cmd==assertionCommands[index]),"assertion command selection differs");
      require(sim.get(a.getPredicate())==legal[legalIndices[index]],"assertion legality predicate differs"); ++index;
      if(enabled&&!sim.get(a.getPredicate()))++illegal; }
    require(index==4&&assertions==unsigned(fire&&!reset&&uses&&cmd>=1&&cmd<=4),"command assertion enable differs");
    auto old=model.delay;model.step(reset,fire,cmd,uses,autoPRE,row,sim.timings);sim.edge();
    require(sim.state.lookup(sim.registers.at("state")).getZExtValue()==model.active,"active transition differs");
    require(sim.state.lookup(sim.registers.at("openRowAddr")).getZExtValue()==model.row,"unreset row differs");
    for(auto [i,n]:llvm::enumerate(std::array<const char*,3>{"nextLegalPRE","nextLegalACT","nextLegalCAS"}))
      require(sim.state.lookup(sim.registers.at(n)).getZExtValue()==model.delay[i],"DownCounter transition differs");
    stalledReset+=reset&&!fire;
    underflows+=fire&&!reset&&uses&&cmd==1&&sim.timings["tRAS"]==0;
    smallerSets+=fire&&!reset&&uses&&cmd==1&&((sim.timings["tRAS"]+127)%128)<old[0];
    ++cases;
  };
  sample(true,true,0,false,false,0);
  // Defaults, open/closed page, commands that do not use this bank, and waits.
  for(unsigned cmd=0;cmd<8;++cmd)for(bool ap:{false,true})for(bool use:{false,true}) {
    sample(false,true,cmd,use,ap,(1u<<26)-1);
    for(unsigned i=0;i<170;++i)sample(false,true,0,false,false,0);
  }
  // Exhaust timing boundaries and reset/fire combinations, including underflow.
  for(unsigned value:{0u,1u,2u,63u,126u,127u}) {
    for(auto n:names)sim.timings[n]=value;
    for(unsigned cmd=0;cmd<8;++cmd)for(bool ap:{false,true})for(bool fire:{false,true})for(bool reset:{false,true})
      sample(reset,fire,cmd,true,ap,unsigned(rng())&((1u<<26)-1));
  }
  for(unsigned i=0;i<40000;++i) {
    for(auto n:names)sim.timings[n]=rng()%128;
    sample(rng()%83==0,rng()%5!=0,rng()%8,rng()%4!=0,rng()%2,rng()&((1u<<26)-1));
  }
  require(illegal&&stalledReset&&underflows&&smallerSets,"missing edge-case coverage");
  auto unchanged=print(root->getOperation()); FModuleOp sentinel=*circuit.getOps<FModuleOp>().begin();
  require(failed(goldengate::materializeFASEDBankStateTracker(circuit,sentinel,error)),"duplicate helper accepted");
  require(sentinel.getName()=="Top"&&unchanged==print(root->getOperation()),"duplicate rejection mutated IR/result");
  if(!output.empty()) {
    // Export the real helper as the circuit root so lowering/lint covers its
    // logic instead of optimizing it away behind the empty preservation fixture.
    circuit.setName(helper.getName());
    std::error_code ec; llvm::raw_fd_ostream stream(output,ec);
    require(!ec,"cannot open helper MLIR output");root->print(stream);stream<<"\n";
  }
  llvm::outs()<<"FASED bank state tracker: "<<cases<<" transitions; "<<illegal<<" illegal-command assertions; "
      <<stalledReset<<" stalled resets; "<<underflows<<" ACT underflow cases; "<<smallerSets<<" smaller-set cases; atomic duplicate rejection\n";
}
}
int main(int argc,char **argv) {
  try { require(argc<=2,"usage: goldengate-fased-bank-state-tracker-test [helper.mlir]");
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(ctx,argc==2?argv[1]:"");return 0; }
  catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}
}
