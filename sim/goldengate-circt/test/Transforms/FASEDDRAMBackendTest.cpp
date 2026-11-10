// See LICENSE for license details.
#include "goldengate/FASEDDRAMBackend.h"
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
using namespace mlir;using namespace circt::firrtl;
namespace {
void require(bool ok,llvm::StringRef message){if(!ok)throw std::runtime_error(message.str());}
std::string print(Operation *op){std::string s;llvm::raw_string_ostream out(s);op->print(out);return s;}
struct Interpreter {
 FModuleOp module;llvm::DenseMap<Value,Value> drivers;llvm::DenseMap<Value,uint64_t> state,memo;
 std::map<std::string,Value> outputs,regs;std::map<std::string,uint64_t> inputs;
 Interpreter(FModuleOp m):module(m){
  for(auto r:m.getOps<RegResetOp>()){state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();}
  for(auto r:m.getOps<RegOp>()){state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();}
  for(auto c:m.getOps<StrictConnectOp>()){drivers[c.getDest()]=c.getSrc();if(auto a=dyn_cast<BlockArgument>(c.getDest()))outputs[m.getPorts()[a.getArgNumber()].getName().str()]=c.getDest();}
 }
 unsigned width(Value v){return *cast<UIntType>(v.getType()).getWidth();}
 uint64_t eval(Value v){
  if(memo.count(v))return memo.lookup(v);auto *op=v.getDefiningOp();uint64_t n;
  if(state.count(v))n=state.lookup(v);
  else if(drivers.count(v))n=eval(drivers.lookup(v));
  else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue().getZExtValue();
  else if(isa_and_nonnull<MuxPrimOp>(op))n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
  else if(isa_and_nonnull<NotPrimOp>(op))n=~eval(op->getOperand(0));
  else if(isa_and_nonnull<AndPrimOp>(op))n=eval(op->getOperand(0))&eval(op->getOperand(1));
  else if(isa_and_nonnull<OrPrimOp>(op))n=eval(op->getOperand(0))|eval(op->getOperand(1));
  else if(isa_and_nonnull<EQPrimOp>(op))n=eval(op->getOperand(0))==eval(op->getOperand(1));
  else if(isa_and_nonnull<AddPrimOp>(op))n=eval(op->getOperand(0))+eval(op->getOperand(1));
  else if(isa_and_nonnull<SubPrimOp>(op))n=eval(op->getOperand(0))-eval(op->getOperand(1));
  else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op))n=eval(bits.getInput())>>bits.getLo();
  else if(isa_and_nonnull<CatPrimOp>(op))n=(eval(op->getOperand(0))<<width(op->getOperand(1)))|eval(op->getOperand(1));
  else if(auto a=dyn_cast<BlockArgument>(v))n=inputs.at(module.getPorts()[a.getArgNumber()].getName().str());
  else throw std::runtime_error("unsupported evaluator op");
  n&=(uint64_t(1)<<width(v))-1;memo[v]=n;return n;
 }
 uint64_t get(const std::string &n){return eval(outputs.at(n));}
 void edge(){llvm::DenseMap<Value,uint64_t> next;for(auto r:module.getOps<RegResetOp>())next[r.getResult()]=eval(r.getResetSignal())?eval(r.getResetValue()):eval(drivers.lookup(r.getResult()));for(auto r:module.getOps<RegOp>())next[r.getResult()]=eval(drivers.lookup(r.getResult()));state=std::move(next);memo.clear();}
};
struct Queue {
 struct Slot{unsigned deadline=0,id=0,len=0;bool pending=false;};std::array<Slot,4> slots{};
 unsigned head=0,tail=0,count=0;
 bool valid(unsigned cycle)const{return count&&(!slots[head].pending||slots[head].deadline==cycle);}
 void step(unsigned cycle,unsigned latency,bool in,unsigned id,unsigned len,bool out,bool reset,bool fire){
  if(!fire)return;bool enq=in&&count<4,deq=out&&valid(cycle);
  // Expiry applies to every physical slot, including invalid/nonhead slots.
  for(auto &slot:slots)if(slot.deadline==cycle)slot.pending=false;
  if(enq)slots[tail]={unsigned((cycle+latency)&4095),id,len,latency!=1};
  if(reset){head=tail=count=0;for(auto &slot:slots)slot.pending=false;return;}
  if(enq)tail=(tail+1)%4;if(deq)head=(head+1)%4;count+=unsigned(enq)-unsigned(deq);
 }
};
void behavior(MLIRContext &ctx,llvm::StringRef output){
 auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
 auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();auto original=print(top);
 FModuleOp helper;std::string error;require(succeeded(goldengate::materializeFASEDDRAMBackend(circuit,helper,error)),error);
 require(succeeded(verify(*root)),"invalid backend IR");require(original==print(top),"preexisting module changed");
 require(helper.getNumPorts()==22,"backend interface differs");
 Interpreter sim(helper);Queue read,write;std::mt19937 rng(0xd1a9beef);
 require(sim.regs.size()==34,"backend register geometry differs");
 require(std::distance(helper.getOps<RegResetOp>().begin(),helper.getOps<RegResetOp>().end())==14,"only pointers/full/pending reset");
 unsigned cases=0,blocked=0,simultaneous=0,zeroAssertions=0,stalledReset=0;
 auto sample=[&](unsigned cycle,unsigned rlat,unsigned wlat,bool rv,bool wv,bool rr,bool wr,bool reset,bool fire){
  unsigned rid=rng()%16,wid=rng()%16,len=rng()%256;sim.memo.clear();
  sim.inputs={{"reset",reset},{"targetFire",fire},{"newReadValid",rv},{"newReadID",rid},{"newReadLen",len},{"newWriteValid",wv},{"newWriteID",wid},{"completedReadReady",rr},{"completedWriteReady",wr},{"readLatency",rlat},{"writeLatency",wlat},{"tCycle",cycle}};
  auto compare=[&](const Queue &q,const char *stem,unsigned idOut){
   bool r=sim.get(std::string("new")+stem+"Ready");bool v=sim.get(std::string("completed")+stem+"Valid");
   require(r==(q.count<4),"queue ready differs");require(v==q.valid(cycle),"deadline validity differs");
   require(sim.get(std::string(stem==std::string("Read")?"read":"write")+"Count")==q.count,"count differs");
   if(v){require(sim.eval(helper.getBodyBlock()->getArgument(idOut))==q.slots[q.head].id,"response ID differs");if(idOut==12)require(sim.get("completedReadLen")==q.slots[q.head].len,"response len differs");}
  };
  compare(read,"Read",12);compare(write,"Write",16);
  unsigned a=0;for(auto assertion:helper.getOps<AssertOp>()){
   bool fail=sim.eval(assertion.getEnable())&&!sim.eval(assertion.getPredicate());
   bool expected=fire&&!reset&&(a==0?(rv&&read.count<4&&rlat==0):(wv&&write.count<4&&wlat==0));require(fail==expected,"latency assertion gating differs");zeroAssertions+=fail;++a;
  }require(a==2,"missing latency assertions");
  blocked+=rv&&read.count==4;simultaneous+=rv&&read.count<4&&rr&&read.valid(cycle);stalledReset+=reset&&!fire;
  read.step(cycle,rlat,rv,rid,len,rr,reset,fire);write.step(cycle,wlat,wv,wid,0,wr,reset,fire);sim.edge();++cases;
 };
 auto clear=[&]{sample(0,1,1,false,false,false,false,true,true);};clear();
 // No flow-through, full queue rejects insertion even when removing its head.
 for(unsigned i=0;i<4;++i)sample(i,1,1,true,true,false,false,false,true);
 require(read.count==4&&write.count==4,"fill failed");sample(4,1,1,true,true,true,true,false,true);require(read.count==3,"full bypass was introduced");
 // An off-head deadline expires, then stays done after the cycle has passed.
 clear();sample(100,20,20,true,false,false,false,false,true);sample(101,2,2,true,false,false,false,false,true);
 sample(103,7,7,false,false,false,false,false,true);sample(120,7,7,false,false,true,false,false,true);
 sample(121,7,7,false,false,false,false,false,true);require(read.valid(121),"nonhead expiry did not latch");
 // A skipped equality must wait for a complete modulo wrap, never >= comparison.
 clear();sample(4090,10,10,true,true,false,false,false,true);sample(3,1,1,false,false,false,false,false,true);
 sample(5,1,1,false,false,false,false,false,true);require(!read.valid(5),"skipped deadline treated as elapsed");
 sample(4,1,1,false,false,false,false,false,true);sample(5,1,1,false,false,true,true,false,true);
 // Enqueue on an already-expired physical slot must override expiry clear.
 clear();sample(0,9,9,true,false,false,false,false,true);sample(1,1,1,false,false,false,false,true,true);
 sample(9,7,7,true,false,false,false,false,true);sample(10,1,1,false,false,false,false,false,true);
 require(!read.valid(10),"expiry won over simultaneous enqueue");
 // Exhaust initial pointer/full boundary cases and all nonzero low latencies.
 for(unsigned latency=1;latency<33;++latency){clear();for(unsigned k=0;k<80;++k)sample((4090+k)&4095,latency,33-latency,k%3!=0,k%5!=0,k%4!=0,k%3!=0,k==37,k%7!=0);}
 // Long reproducible stress includes arbitrary cycle jumps, modulo deadlines,
 // latency0 assertion failures, independent backpressure and stalled resets.
 unsigned cycle=0;for(unsigned k=0;k<40000;++k){if(k%97==0)cycle=rng()%4096;else cycle=(cycle+1)&4095;sample(cycle,rng()%4096,rng()%4096,rng()%2,rng()%2,rng()%2,rng()%2,rng()%257==0,rng()%5!=0);}
 // Force latency0 assertion coverage (simulation records failures, never aborts).
 clear();sample(0,0,0,true,true,false,false,false,true);sample(1,0,0,true,true,false,false,true,false);
 require(blocked&&simultaneous&&zeroAssertions&&stalledReset,"missing boundary coverage");
 auto unchanged=print(root->getOperation());FModuleOp sentinel=top;require(failed(goldengate::materializeFASEDDRAMBackend(circuit,sentinel,error)),"duplicate accepted");require(sentinel==top&&unchanged==print(root->getOperation()),"duplicate rejection mutated IR/result");
 if(!output.empty()){circuit.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream os(output,ec);require(!ec,"cannot write fixture");root->print(os);os<<"\n";}
 llvm::outs()<<"FASED DRAM backend: "<<cases<<" transitions; "<<blocked<<" full rejects; "<<simultaneous<<" simultaneous operations; "<<zeroAssertions<<" gated latency assertions; "<<stalledReset<<" stalled resets; modulo/skipped/nonhead deadlines and enqueue priority pass\n";
}
}
int main(int argc,char **argv){try{require(argc<=2,"usage: goldengate-fased-dram-backend-test [helper.mlir]");MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(ctx,argc==2?argv[1]:"");return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}}
