// See LICENSE for license details.
#include "goldengate/FASEDLLCMSHRTable.h"
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
void require(bool ok, llvm::StringRef message) {if(!ok)throw std::runtime_error(message.str());}
std::string print(Operation *op) {std::string s;llvm::raw_string_ostream os(s);op->print(os);return s;}
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value,Value> drivers;
  llvm::DenseMap<Value,uint64_t> state,memo;
  std::map<std::string,Value> outputs,regs;
  std::map<std::string,uint64_t> inputs;
  Interpreter(FModuleOp module):module(module) {
    for(auto r:module.getOps<RegResetOp>()) {
      state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();
    }
    for(auto r:module.getOps<RegOp>()) {
      state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();
    }
    for(auto c:module.getOps<StrictConnectOp>()) {
      drivers[c.getDest()]=c.getSrc();
      if(!state.count(c.getDest()))outputs[path(c.getDest())]=c.getDest();
    }
  }
  std::string path(Value value) {
    if(auto arg=dyn_cast<BlockArgument>(value))
      return module.getPorts()[arg.getArgNumber()].getName().str();
    if(auto f=value.getDefiningOp<SubfieldOp>())return path(f.getInput())+"."+f.getFieldName().str();
    if(auto i=value.getDefiningOp<SubindexOp>())return path(i.getInput())+"["+std::to_string(i.getIndex())+"]";
    throw std::runtime_error("unexpected external path");
  }
  unsigned width(Value value) {return *cast<UIntType>(value.getType()).getWidth();}
  uint64_t eval(Value value) {
    if(memo.count(value))return memo.lookup(value);
    auto *op=value.getDefiningOp();uint64_t n=0;
    if(state.count(value))n=state.lookup(value);
    else if(drivers.count(value))n=eval(drivers.lookup(value));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue().getZExtValue();
    else if(isa_and_nonnull<MuxPrimOp>(op))n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
    else if(isa_and_nonnull<AndPrimOp>(op))n=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if(isa_and_nonnull<OrPrimOp>(op))n=eval(op->getOperand(0))|eval(op->getOperand(1));
    else if(isa_and_nonnull<NotPrimOp>(op))n=~eval(op->getOperand(0));
    else if(isa_and_nonnull<EQPrimOp>(op))n=eval(op->getOperand(0))==eval(op->getOperand(1));
    else if(isa_and_nonnull<AddPrimOp>(op))n=eval(op->getOperand(0))+eval(op->getOperand(1));
    else if(isa_and_nonnull<GTPrimOp>(op))n=eval(op->getOperand(0))>eval(op->getOperand(1));
    else if(isa_and_nonnull<LTPrimOp>(op))n=eval(op->getOperand(0))<eval(op->getOperand(1));
    else if(isa_and_nonnull<PadPrimOp>(op))n=eval(op->getOperand(0));
    else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op))n=eval(bits.getInput())>>bits.getLo();
    else n=inputs.at(path(value));
    n&=(uint64_t(1)<<width(value))-1;memo[value]=n;return n;
  }
  uint64_t get(const std::string &name){return eval(outputs.at(name));}
  void edge() {
    llvm::DenseMap<Value,uint64_t> next;
    for(auto r:module.getOps<RegResetOp>())
      next[r.getResult()]=eval(r.getResetSignal())?eval(r.getResetValue()):eval(drivers.lookup(r.getResult()));
    for(auto r:module.getOps<RegOp>())next[r.getResult()]=eval(drivers.lookup(r.getResult()));
    state=std::move(next);memo.clear();
  }

};
constexpr std::array<const char *,8> fields{"setAddr","id","len","isWrite","addr","writeInFlight","readInFlight","enabled"};
using Entry=std::array<uint64_t,8>;
struct Model {
 std::array<Entry,8> entries{};unsigned previousActive=0;
 bool valid(unsigned i)const{return (entries[i][5]||entries[i][6])&&entries[i][7];}
 bool free(unsigned i)const{return !valid(i)&&entries[i][7];}
 unsigned count()const{unsigned n=0;for(unsigned i=0;i<8;++i)n+=valid(i);return n;}
 bool available()const{for(unsigned i=0;i<8;++i)if(free(i))return true;return false;}
 unsigned next()const{for(unsigned i=0;i<8;++i)if(free(i))return i;return 7;}
 bool collision(unsigned set)const{for(unsigned i=0;i<8;++i)if(valid(i)&&entries[i][0]==set)return true;return false;}
};
void behavior(MLIRContext &ctx,llvm::StringRef output){
 auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
 auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();auto original=print(top);
 FModuleOp helper;std::string error;require(succeeded(goldengate::materializeFASEDLLCMSHRTable(circuit,helper,error)),error);
 require(succeeded(verify(*root)),"invalid MSHR IR");require(original==print(top),"preexisting module changed");require(helper.getNumPorts()==28,"MSHR interface differs");
 Interpreter sim(helper);Model model;std::mt19937_64 rng(0x11c8e57);unsigned cases=0,full=0,alias=0,priority=0,stalledReset=0,disabledBusy=0,assertionFailures=0;
 require(sim.regs.size()==65,"MSHR register geometry differs");
 require(std::distance(helper.getOps<RegResetOp>().begin(),helper.getOps<RegResetOp>().end())==24,"MSHR reset fields differ");
 auto sample=[&](bool reset,bool fire,unsigned active,bool allocate,unsigned set,unsigned id,unsigned len,bool isWrite,uint64_t addr,bool rd,bool wr,bool cr,unsigned cri,bool cw,unsigned cwi,unsigned readSet,unsigned writeSet){
  sim.memo.clear();sim.inputs={{"reset",reset},{"targetFire",fire},{"activeMSHRs",active},{"allocate",allocate},{"allocateSet",set},{"allocateId",id},{"allocateLen",len},{"allocateIsWrite",isWrite},{"allocateAddr",addr},{"allocateRead",rd},{"allocateWrite",wr},{"completedRead",cr},{"completedReadId",cri},{"completedWrite",cw},{"completedWriteId",cwi},{"readSet",readSet},{"writeSet",writeSet}};
  require(sim.get("available")==model.available(),"available differs");require(sim.get("nextIndex")==model.next(),"priority index differs");require(sim.get("allocated")==model.count(),"allocated count differs");
  require(sim.get("readCollision")==model.collision(readSet),"read collision differs");require(sim.get("writeCollision")==model.collision(writeSet),"write collision differs");
  for(unsigned j=1;j<5;++j)require(sim.get(std::array<const char *,4>{"responseId","responseLen","responseIsWrite","responseAddr"}[j-1])==model.entries[cri&7][j],"response metadata differs");
  for(unsigned i=0;i<8;++i)for(unsigned j=0;j<8;++j)require(sim.get("entries["+std::to_string(i)+"]."+fields[j])==model.entries[i][j],"entry field differs");
  unsigned ai=0;for(auto assertion:helper.getOps<AssertOp>()){
   bool expected=fire&&!reset&&!(ai==0?(model.count()<model.previousActive||!model.available()):(model.count()==model.previousActive||model.available()));
   bool failed=sim.eval(assertion.getEnable())&&!sim.eval(assertion.getPredicate());require(failed==expected,"runtime capacity assertion differs");assertionFailures+=failed;++ai;
  }require(ai==2,"missing runtime capacity assertions");
  unsigned next=model.next();full+=!model.available();alias+=(cr&&cri>=8)||(cw&&cwi>=8);priority+=allocate&&((cr&&((cri&7)==next))||(cw&&((cwi&7)==next)));stalledReset+=reset&&!fire;
  for(unsigned i=0;i<8;++i)disabledBusy+=!model.entries[i][7]&&(model.entries[i][5]||model.entries[i][6]);
  if(fire){
   if(allocate){auto &e=model.entries[next];e[0]=set;e[1]=id;e[2]=len;e[3]=isWrite;e[4]=addr;e[5]=wr;e[6]=rd;}
   if(cr)model.entries[cri&7][6]=0;if(cw)model.entries[cwi&7][5]=0;
   for(unsigned i=0;i<8;++i){if(reset){model.entries[i][5]=model.entries[i][6]=0;}model.entries[i][7]=reset||i<active;}
   model.previousActive=active;
  }
  sim.edge();++cases;
 };
 auto directed=[&](bool reset,bool fire,unsigned active,bool allocate,bool cr,unsigned cri,bool cw,unsigned cwi){sample(reset,fire,active,allocate,23,11,197,true,(uint64_t(1)<<34)|0x1007,true,true,cr,cri,cw,cwi,23,24);};
 directed(true,true,8,false,false,0,false,0);
 for(unsigned i=0;i<8;++i)directed(false,true,8,true,false,0,false,0);
 require(!model.available()&&model.next()==7&&model.count()==8,"full table boundary differs");
 directed(false,true,8,false,true,8,false,0);require(model.entries[0][6]==0&&model.entries[0][5]==1,"read alias clear or independent flag differs");
 directed(false,true,8,false,false,0,true,8);require(model.next()==0,"write alias clear differs");
 directed(false,true,8,true,true,8,true,8);require(model.entries[0][5]==0&&model.entries[0][6]==0,"completion must override allocation");
 directed(false,true,1,false,false,0,false,0);require(model.count()==0,"disabled MSHRs count as valid");
 directed(false,true,8,false,false,0,false,0);require(model.count()==7,"reenabling lost busy flags");
 // Payload allocation remains live on reset; flight flags reset, enabled resets
 // true, and the subsequent nonreset target edge applies the runtime limit.
 directed(true,false,0,true,false,0,false,0);
 sample(true,true,0,true,1207,5,117,false,(uint64_t(1)<<33)|99,true,true,false,0,false,0,1207,23);require(model.available()&&model.entries[0][1]==5&&model.entries[0][2]==117,"reset payload/allocation semantics differ");
 directed(false,true,0,false,false,0,false,0);require(!model.available()&&model.next()==7,"zero-active boundary differs");
 for(unsigned i=0;i<24000;++i){
  unsigned set=rng()%4096,readSet=rng()%2?set:23,writeSet=rng()%2?set:23;
  sample(rng()%173==0,rng()%5!=0,rng()%16,rng()%2,set,rng()%16,rng()%256,rng()%2,rng()&((uint64_t(1)<<35)-1),rng()%2,rng()%2,rng()%2,rng()%16,rng()%2,rng()%16,readSet,writeSet);
 }
 require(full&&alias&&priority&&stalledReset&&disabledBusy&&assertionFailures,"missing MSHR boundary coverage");
 auto unchanged=print(root->getOperation());FModuleOp sentinel=top;require(failed(goldengate::materializeFASEDLLCMSHRTable(circuit,sentinel,error)),"duplicate accepted");require(sentinel==top&&unchanged==print(root->getOperation()),"duplicate rejection mutated IR/result");
 if(!output.empty()){circuit.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream os(output,ec);require(!ec,"cannot write fixture");root->print(os);os<<"\n";}
 llvm::outs()<<"FASED LLC MSHRs: "<<cases<<" transitions; "<<full<<" full cycles; "<<alias<<" alias IDs; "<<priority<<" allocation/completion overlaps; "<<stalledReset<<" stalled resets; registered enables, independent completion flags, collision masks and reset-time metadata writes pass\n";
}
}
int main(int argc,char **argv){try{require(argc<=2,"usage: goldengate-fased-llc-mshr-table-test [helper.mlir]");MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(ctx,argc==2?argv[1]:"");return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}}
