// See LICENSE for license details.
#include "goldengate/FASEDUnifiedTransactionScheduler.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <deque>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef text){if(!ok)throw std::runtime_error(text.str());}
std::string print(Operation *op){std::string s;llvm::raw_string_ostream os(s);op->print(os);return s;}
struct Interpreter {
  FModuleOp module;
  llvm::DenseMap<Value,Value> drivers;
  llvm::DenseMap<Value,uint64_t> state,memo;
  std::map<std::string,Value> outputs,registers;
  std::map<std::string,uint64_t> inputs;
  Interpreter(FModuleOp module):module(module) {
    for(auto r:module.getOps<RegResetOp>()) {
      state[r.getResult()]=0;registers[r.getName().str()]=r.getResult();
    }
    for(auto r:module.getOps<RegOp>()) {
      state[r.getResult()]=0;registers[r.getName().str()]=r.getResult();
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
    else if(isa_and_nonnull<NEQPrimOp>(op))n=eval(op->getOperand(0))!=eval(op->getOperand(1));
    else if(isa_and_nonnull<LEQPrimOp>(op))n=eval(op->getOperand(0))<=eval(op->getOperand(1));
    else if(isa_and_nonnull<GEQPrimOp>(op))n=eval(op->getOperand(0))>=eval(op->getOperand(1));
    else if(isa_and_nonnull<AddPrimOp>(op))n=eval(op->getOperand(0))+eval(op->getOperand(1));
    else if(isa_and_nonnull<SubPrimOp>(op))n=eval(op->getOperand(0))-eval(op->getOperand(1));
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

struct Request {uint64_t id,len,isWrite,addr;};
void behavior(MLIRContext &ctx,unsigned capacity,llvm::StringRef output) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
  auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();
  auto before=print(top);FModuleOp helper;std::string error;
  require(succeeded(goldengate::materializeFASEDUnifiedTransactionScheduler(circuit,capacity,helper,error)),error);
  require(succeeded(verify(*root)),"invalid unified scheduler IR");
  require(print(top)==before&&circuit.getName()=="Top","changed preexisting circuit");
  require(helper.getNumPorts()==24,"incorrect port count");
  for(unsigned i:{16u,17u})require(*cast<UIntType>(helper.getPorts()[i].type).getWidth()==capacity+1,"pending counter width mismatch");
  Interpreter sim(helper);std::deque<Request> queue;unsigned completed=0;bool lastGrant=true;
  sim.state[sim.registers.at("lastGrant")]=1;
  std::mt19937_64 rng(0xa11b17+capacity);
  unsigned cycles=0,acceptedR=0,acceptedW=0,released=0,fullReject=0,blocked=0,stalledReset=0,simultaneous=0,saturated=0,resetGrant=0;
  auto sample = [&](bool reset,
                    bool fire,
                    bool ar,
                    bool aw,
                    bool w,
                    bool last,
                    bool ready,
                    uint64_t pendingW,
                    uint64_t pendingAW) {
    sim.memo.clear();
    Request read{rng() % 16, rng() % 256, 0, rng() & ((1ULL << 35) - 1)},
        write{rng() % 16, rng() % 256, 1, rng() & ((1ULL << 35) - 1)};
    sim.inputs={{"reset",reset},{"targetFire",fire},{"arValid",ar},{"arId",read.id},{"arLen",read.len},{"arAddr",read.addr},
      {"awValid",aw},{"awId",write.id},{"awLen",write.len},{"awAddr",write.addr},{"wValid",w},{"wLast",last},{"pendingWReq",pendingW},{"pendingAWReq",pendingAW},{"nextReady",ready}};
    // Independent two-way round-robin reference: after a successful grant,
    // first search strictly after the last winner, then wrap to AR and AW.
    bool selectAW=(aw&&!lastGrant)||!ar,space=queue.size()<8;
    bool expectAR=space&&!(aw&&!lastGrant);
    bool expectAW=space&&(!lastGrant||!ar);
    bool valid=!queue.empty()&&(!queue.front().isWrite||completed!=0);
    bool wready=pendingW<=pendingAW;
    require(sim.get("arReady")==expectAR&&sim.get("awReady")==expectAW,"RR ready differs");
    require(sim.get("wReady")==wready,"write data ready differs");
    require(sim.get("nextValid")==valid,"head gating differs");
    if(valid){
      auto r = queue.front();
      require(sim.get("nextId") == r.id && sim.get("nextLen") == r.len &&
                  sim.get("nextIsWrite") == r.isWrite &&
                  sim.get("nextAddr") == r.addr,
              "queue metadata differs");
    }
    bool enq=space&&(ar||aw),deq=valid&&ready;
    bool add=w&&last&&wready,sub=deq&&queue.front().isWrite;
    if(fire){
      if(enq)
        lastGrant = selectAW;
      if(reset){
        queue.clear();
        completed = 0;
      } else {
        if(deq){
          queue.pop_front();
          ++released;
        }
        if(enq){
          queue.push_back(selectAW ? write : read);
          if (selectAW)
            ++acceptedW;
          else
            ++acceptedR;
        }
        if(add&&!sub&&completed<capacity)
          ++completed;
        else if(!add&&sub&&completed>0)
          --completed;
      }
    }
    fullReject+=!space&&(ar||aw);
    blocked += !queue.empty() && queue.front().isWrite && !completed;
    stalledReset+=reset&&!fire;
    simultaneous += fire && add && sub;
    saturated += completed == capacity;
    resetGrant += fire && reset && enq;
    sim.edge();
    require(sim.state[sim.registers.at("completedWrites")] == completed,
            "completed write counter differs");
    require(sim.state[sim.registers.at("lastGrant")]==lastGrant,"RR reset incorrectly changed winner");
    ++cycles;
  };
  sample(true,true,false,false,false,false,false,0,0);
  // Fill from AR/AW while preventing release and exercise full rejection.
  for(unsigned i=0;i<16;++i)sample(false,true,true,true,false,false,false,0,0);
  // Saturate completed data and then drain, covering lack of same-cycle bypass.
  for(unsigned i=0;i<capacity+4;++i)sample(false,true,false,false,true,true,false,0,0);
  for(unsigned i=0;i<32;++i)sample(false,true,false,false,true,true,true,0,0);
  sample(true,true,true,true,true,true,true,0,0);
  sample(true,false,true,true,true,true,true,0,0);
  for(unsigned i=0;i<40000;++i)
    sample(rng() % 97 == 0,
           rng() % 5 != 0,
           rng() % 3 != 0,
           rng() % 3 != 0,
           rng() % 2,
           rng() % 4 != 0,
           rng() % 3 != 0,
           rng() & ((1ULL << (capacity + 1)) - 1),
           rng() & ((1ULL << (capacity + 1)) - 1));
  require(acceptedR&&acceptedW&&released&&fullReject&&blocked&&stalledReset&&simultaneous&&saturated&&resetGrant,"missing boundary coverage");
  auto unchanged=print(root->getOperation());FModuleOp sentinel=top;
  require(failed(goldengate::materializeFASEDUnifiedTransactionScheduler(circuit,capacity,sentinel,error)),"duplicate accepted");
  require(sentinel==top&&print(root->getOperation())==unchanged,"duplicate failure changed circuit/result");
  require(failed(goldengate::materializeFASEDUnifiedTransactionScheduler(
              circuit, 33, sentinel, error)),
          "unsupported capacity accepted");
  require(sentinel==top&&print(root->getOperation())==unchanged,"unsupported failure changed circuit/result");
  auto external=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} firrtl.extmodule @GGFASEDUnifiedTransactionScheduler"+std::to_string(capacity)+"() } }",&ctx);
  auto other=*external->getOps<CircuitOp>().begin();unchanged=print(external->getOperation());
  require(failed(goldengate::materializeFASEDUnifiedTransactionScheduler(other,capacity,sentinel,error)),"external collision accepted");
  require(sentinel==top&&print(external->getOperation())==unchanged,"external collision changed IR/result");
  if(!output.empty()){circuit.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream stream(output,ec);require(!ec,"cannot open fixture");root->print(stream);stream<<"\n";}
  llvm::outs()<<"FASED unified scheduler "<<capacity<<": "<<cycles<<" cycles; AR="<<acceptedR<<" AW="<<acceptedW<<" released="<<released<<" fullReject="<<fullReject<<" blocked="<<blocked<<" simultaneous="<<simultaneous<<" saturation="<<saturated<<" resetGrant="<<resetGrant<<"\n";
}
}
int main(int argc,char **argv){try{
  require(argc==1||argc==3,"usage: goldengate-fased-unified-transaction-scheduler-test [10|20 helper.mlir]");
  MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  if (argc == 3)
    behavior(ctx, std::stoul(argv[1]), argv[2]);
  else
    for (unsigned capacity : {10u, 16u, 20u, 32u})
      behavior(ctx, capacity, "");
  return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}}
