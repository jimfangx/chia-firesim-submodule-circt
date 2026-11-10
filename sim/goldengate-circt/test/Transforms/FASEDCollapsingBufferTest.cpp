// See LICENSE for license details.
#include "goldengate/FASEDCollapsingBuffer.h"
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
constexpr std::array<const char *,10> names{
    "id","len","isWrite","rowAddr","bankAddrOH","bankAddr",
    "rankAddrOH","rankAddr","isReady","mayPRE"};
constexpr std::array<unsigned,10> widths{4,8,1,26,8,3,4,2,1,1};
void require(bool ok, llvm::StringRef message) {
  if(!ok)throw std::runtime_error(message.str());
}
std::string print(Operation *op) {
  std::string result;llvm::raw_string_ostream os(result);op->print(os);return result;
}
struct Entry {
  bool valid=false;
  std::array<uint64_t,10> bits{};
};
using Entries=std::array<Entry,8>;
std::string slot(const char *port,unsigned i) {
  return std::string(port)+"["+std::to_string(i)+"]";
}
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
  void set(const std::string &name,const Entry &entry) {
    inputs[name+".valid"]=entry.valid;
    for(unsigned j=0;j<names.size();++j)inputs[name+".bits."+names[j]]=entry.bits[j];
  }
  void compare(const Entries &entries) {
    for(unsigned i=0;i<8;++i) {
      require(get(slot("entries",i)+".valid")==entries[i].valid,"entry valid differs");
      for(unsigned j=0;j<names.size();++j)
        require(get(slot("entries",i)+".bits."+names[j])==entries[i].bits[j],"entry bits differ");
    }
  }
};
// Scala linkEntries is a recursion over oldest-to-youngest updates. Reproduce
// that literal reference here, independently of FIRRTL register construction.
void linkReference(unsigned i,bool shifting,unsigned depth,const Entries &updates,
                   const Entry &enq,Entries &next,bool &ready) {
  bool shift=shifting||!updates[i].valid;
  if(i==7) {
    next[i]=shift?enq:updates[i];ready=shift;
  } else {
    // take(8).reverse on the one-hot vector selects the (7-depth) slot.
    if(depth==7-i)shift=false;
    next[i]=shift?updates[i+1]:updates[i];
    linkReference(i+1,shift,depth,updates,enq,next,ready);
  }
}
void behavior(MLIRContext &ctx,llvm::StringRef output) {
  auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
  auto circuit=*root->getOps<CircuitOp>().begin();
  circuit->setAttr("test.keep",StringAttr::get(&ctx,"identity"));
  auto top=*circuit.getOps<FModuleOp>().begin();auto before=print(top);
  FModuleOp helper;std::string error;
  require(succeeded(goldengate::materializeFASEDCollapsingBuffer(circuit,helper,error)),error);
  require(succeeded(verify(*root)),"invalid helper IR");
  require(before==print(top)&&circuit.getName()=="Top","changed preexisting top");
  require(circuit->getAttrOfType<StringAttr>("test.keep").getValue()=="identity","changed circuit metadata");
  require(helper.getNumPorts()==9,"incorrect port count");
  auto entryType=cast<BundleType>(helper.getPorts()[5].type);
  require(entryType.getElements().size()==10,"incorrect entry fields");
  for(unsigned j=0;j<names.size();++j) {
    auto f=entryType.getElements()[j];
    require(f.name==names[j]&&*cast<UIntType>(f.type).getWidth()==widths[j],"incorrect selected entry geometry");
  }
  require(*cast<UIntType>(helper.getPorts()[8].type).getWidth()==4,"dynamic depth must retain four bits");
  require(cast<FVectorType>(helper.getPorts()[6].type).getNumElements()==8,"incorrect depth");
  Interpreter sim(helper);Entries model;
  require(sim.registers.size()==88,"incorrect register count");
  require(std::distance(helper.getOps<RegResetOp>().begin(),helper.getOps<RegResetOp>().end())==8,"only valid bits reset");
  std::mt19937_64 rng(0xc011a953);
  auto randomEntry=[&]() {
    Entry e;e.valid=rng()%2;
    for(unsigned j=0;j<names.size();++j)e.bits[j]=rng()&((uint64_t(1)<<widths[j])-1);
    return e;
  };
  // Arbitrary initial data checks that reset never erases a request's bits.
  for(unsigned i=0;i<8;++i) {
    model[i]=randomEntry();model[i].valid=false;
    for(unsigned j=0;j<names.size();++j)
      sim.state[sim.registers.at("entry"+std::to_string(i)+"_"+names[j])]=model[i].bits[j];
  }
  unsigned cases=0,stalledResets=0,fullRejects=0,depthBarriers=0;
  auto sample=[&](bool reset,bool fire,unsigned depth,const Entries &updates,const Entry &enq) {
    sim.memo.clear();sim.inputs["reset"]=reset;sim.inputs["targetFire"]=fire;
    sim.inputs["enqValid"]=enq.valid;sim.inputs["programmableDepth"]=depth;
    for(unsigned j=0;j<names.size();++j)sim.inputs[std::string("enqBits.")+names[j]]=enq.bits[j];
    for(unsigned i=0;i<8;++i)sim.set(slot("updates",i),updates[i]);
    sim.compare(model);
    Entries next;bool ready=false;linkReference(0,false,depth,updates,enq,next,ready);
    require(sim.get("enqReady")==ready,"enqueue ready differs");
    // Ready is combinational even when targetFire/reset prevent state advance.
    if(fire) {
      model=next;if(reset)for(auto &e:model)e.valid=false;
    }
    sim.edge();sim.compare(model);
    stalledResets+=reset&&!fire;fullRejects+=!ready;depthBarriers+=depth<8;
    ++cases;
  };
  Entry enq=randomEntry();enq.valid=true;
  Entries updates{};
  // With depth 8 the first arrival lands in slot 7, then advances one slot per
  // target cycle. No same-cycle ripple of the newly arrived value is allowed.
  sample(true,true,8,model,enq);
  updates=model;for(auto &e:updates)e.valid=false;
  sample(false,true,8,updates,enq);
  require(model[7].valid&&!model[6].valid,"first request did not enter youngest slot");
  Entry absent=enq;absent.valid=false;
  for(unsigned k=0;k<7;++k)sample(false,true,8,model,absent);
  require(model[0].valid,"request did not reach oldest slot after seven target cycles");
  // A depth-7 boundary is at the oldest slot, so even its vacancy cannot pull
  // from slot 1. The depth-0 bit lies at slot 7 and is ignored by the base case.
  updates=model;updates[0].valid=false;updates[1].valid=true;
  sample(false,true,7,updates,absent);
  require(!model[0].valid,"reversed one-hot oldest boundary was normalized away");
  updates=model;for(auto &e:updates)e.valid=false;
  sample(false,true,0,updates,enq);
  require(model[7].valid,"youngest base case unexpectedly obeyed lastEntry");
  // Exhaust dynamic depth including out-of-range encodings, every validity
  // pattern, enqueue validity, and reset/fire combinations. Payload changes
  // deliberately include invalid slots because Scala updates all data bits.
  for(unsigned depth=0;depth<16;++depth)for(unsigned mask=0;mask<256;++mask) {
    for(unsigned i=0;i<8;++i) {updates[i]=randomEntry();updates[i].valid=(mask>>i)&1;}
    for(unsigned controls=0;controls<8;++controls) {
      enq=randomEntry();enq.valid=controls&1;
      sample(controls&2,controls&4,depth,updates,enq);
    }
  }
  // Exercise scheduler-style default updates, kills, rewrites, and backpressure
  // through many consecutive target cycles with live occupancy.
  for(unsigned k=0;k<12000;++k) {
    updates=model;
    for(auto &e:updates) {
      if(rng()%11==0)e.valid=false;
      if(rng()%5==0)e.bits=randomEntry().bits;
    }
    sample(rng()%79==0,rng()%5!=0,rng()%16,updates,randomEntry());
  }
  require(stalledResets&&fullRejects&&depthBarriers,"missing boundary coverage");
  auto unchanged=print(root->getOperation());FModuleOp sentinel=top;
  require(failed(goldengate::materializeFASEDCollapsingBuffer(circuit,sentinel,error)),"duplicate symbol accepted");
  require(sentinel==top&&print(root->getOperation())==unchanged,"duplicate rejection mutated circuit/result");
  if(!output.empty()) {
    circuit.setName(helper.getName());
    std::error_code ec;llvm::raw_fd_ostream stream(output,ec);
    require(!ec,"cannot open helper MLIR output");root->print(stream);stream<<"\n";
  }
  llvm::outs()<<"FASED collapsing buffer: "<<cases<<" transitions; "<<stalledResets
      <<" stalled resets; "<<fullRejects<<" enqueue rejections; all 16 dynamic depths; "
      <<"unreset data; atomic duplicate rejection\n";
}
}
int main(int argc,char **argv) {
  try {
    require(argc<=2,"usage: goldengate-fased-collapsing-buffer-test [helper.mlir]");
    MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    behavior(ctx,argc==2?argv[1]:"");return 0;
  } catch(const std::exception &error) {llvm::errs()<<error.what()<<"\n";return 1;}
}
