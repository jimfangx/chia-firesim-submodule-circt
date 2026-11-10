// See LICENSE for license details.
#include "goldengate/FASEDLLCMetadata.h"
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
 FModuleOp module;llvm::DenseMap<Value,uint64_t> state,memo;
 std::map<std::string,Value> drivers,regs;std::map<std::string,uint64_t> inputs;
 std::string path(Value v){
  if(auto a=dyn_cast<BlockArgument>(v))return module.getPorts()[a.getArgNumber()].getName().str();
  if(auto s=v.getDefiningOp<SubfieldOp>())return path(s.getInput())+"."+s.getFieldName().str();
  if(auto s=v.getDefiningOp<SubindexOp>())return path(s.getInput())+"."+std::to_string(s.getIndex());
  if(auto m=v.getDefiningOp<MemOp>())return cast<OpResult>(v).getResultNumber()?"write":"read";
  if(auto r=v.getDefiningOp<RegOp>())return r.getName().str();
  if(auto r=v.getDefiningOp<RegResetOp>())return r.getName().str();
  return "";
 }
 Interpreter(FModuleOp m):module(m){
  for(auto r:m.getOps<RegResetOp>()){state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();}
  for(auto r:m.getOps<RegOp>()){state[r.getResult()]=0;regs[r.getName().str()]=r.getResult();}
  for(auto c:m.getOps<StrictConnectOp>())drivers[path(c.getDest())]=c.getSrc();
 }
 unsigned width(Value v){return *cast<UIntType>(v.getType()).getWidth();}
 uint64_t eval(Value v){
  if(memo.count(v))return memo.lookup(v);auto *op=v.getDefiningOp();uint64_t n;auto key=path(v);
  if(state.count(v))n=state.lookup(v);
  else if(!key.empty()&&inputs.count(key))n=inputs.at(key);
  else if(!key.empty()&&drivers.count(key))n=eval(drivers.at(key));
  else if(auto c=dyn_cast_or_null<ConstantOp>(op))n=c.getValue().getZExtValue();
  else if(isa_and_nonnull<MuxPrimOp>(op))n=eval(op->getOperand(eval(op->getOperand(0))?1:2));
  else if(isa_and_nonnull<NotPrimOp>(op))n=~eval(op->getOperand(0));
  else if(isa_and_nonnull<AndPrimOp>(op))n=eval(op->getOperand(0))&eval(op->getOperand(1));
  else if(isa_and_nonnull<OrPrimOp>(op))n=eval(op->getOperand(0))|eval(op->getOperand(1));
  else if(isa_and_nonnull<XorPrimOp>(op))n=eval(op->getOperand(0))^eval(op->getOperand(1));
  else if(isa_and_nonnull<EQPrimOp>(op))n=eval(op->getOperand(0))==eval(op->getOperand(1));
  else if(isa_and_nonnull<DShlPrimOp>(op))n=eval(op->getOperand(0))<<eval(op->getOperand(1));
  else if(isa_and_nonnull<PadPrimOp>(op))n=eval(op->getOperand(0));
  else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op))n=eval(bits.getInput())>>bits.getLo();
  else if(isa_and_nonnull<CatPrimOp>(op))n=(eval(op->getOperand(0))<<width(op->getOperand(1)))|eval(op->getOperand(1));
  else throw std::runtime_error("unsupported evaluator op or leaf: "+key);
  n&=(uint64_t(1)<<width(v))-1;memo[v]=n;return n;
 }
 uint64_t get(const std::string &n){return eval(drivers.at(n));}
 void edge(){llvm::DenseMap<Value,uint64_t> next;for(auto r:module.getOps<RegResetOp>())next[r.getResult()]=eval(r.getResetSignal())?eval(r.getResetValue()):eval(drivers.at(r.getName().str()));for(auto r:module.getOps<RegOp>())next[r.getResult()]=eval(drivers.at(r.getName().str()));state=std::move(next);memo.clear();}
};
struct Entry{uint32_t tag;bool valid,dirty;};
void behavior(MLIRContext &ctx,llvm::StringRef output){
 auto root=parseSourceString<ModuleOp>("module { firrtl.circuit \"Top\" { firrtl.module @Top() {} } }",&ctx);
 auto circuit=*root->getOps<CircuitOp>().begin();auto top=*circuit.getOps<FModuleOp>().begin();auto original=print(top);
 FModuleOp helper;std::string error;require(succeeded(goldengate::materializeFASEDLLCMetadata(circuit,helper,error)),error);
 require(succeeded(verify(*root)),"invalid LLC metadata IR");require(original==print(top),"preexisting module changed");
 require(helper.getNumPorts()==18,"metadata interface differs");
 auto memory=*helper.getOps<MemOp>().begin();
 require(memory.getDepth()==4096&&memory.getReadLatency()==0&&memory.getWriteLatency()==1,"metadata geometry/latency differs");
 require(memory.getRuw()==RUWAttr::Undefined,"metadata read-under-write differs");
 require(memory.getNumResults()==2,"metadata memory port count differs");
 require(std::distance(helper.getOps<RegResetOp>().begin(),helper.getOps<RegResetOp>().end())==1,"metadata invented reset state");
 require(std::distance(helper.getOps<RegOp>().begin(),helper.getOps<RegOp>().end())==26,"stage1 address/read-data register geometry differs");
 auto type=cast<BundleType>(memory.getResult(0).getType());
 auto metadataType=cast<FVectorType>(type.getElementType("data"));require(metadataType.getNumElements()==8,"metadata way count differs");
 auto entryType=cast<BundleType>(metadataType.getElementType());
 require(cast<UIntType>(entryType.getElementType("tag")).getWidth()==32,"stored tag width differs");
 // Supply arbitrary retained read-data snapshots and independent asynchronous
 // memory data. Check selection/write logic, enabled read-data captures/holds
 // and scalar registers; complete memory sequencing belongs to LLC RTL replay.
 Interpreter sim(helper);std::mt19937_64 rng(0x11c032);
 unsigned cases=0,hits=0,empty=0,evictions=0,dirtyEvictions=0,highTag=0,resetWrites=0,stalls=0;
 for(unsigned k=0;k<18000;++k){
  std::array<Entry,8> entries,raw;for(auto &e:entries)e={uint32_t(rng()),bool(rng()%2),bool(rng()%2)};
  for(auto &e:raw)e={uint32_t(rng()),bool(rng()%2),bool(rng()%2)};
  unsigned ways=k%4,set=rng()%4096,lookupSet=rng()%4096;uint64_t tag=rng()&((uint64_t(1)<<35)-1),lookupTag=rng()&((uint64_t(1)<<35)-1);
  // Alternate forced hit, compulsory miss, full replacement, high-tag false hit.
  if(k%4==0){tag=entries[rng()%(1u<<ways)].tag;for(auto &e:entries)e.valid=true;}
  if(k%4==1){for(auto &e:entries)e.valid=false;}
  if(k%4==2){for(auto &e:entries)e.valid=true;}
  if(k%4==3){for(auto &e:entries)e.valid=true;tag=uint64_t(entries[0].tag)|(uint64_t(1)<<34);++highTag;}
  unsigned lfsr=rng()%65536;bool write=rng()%2,valid=rng()%2,fire=rng()%5!=0,reset=rng()%7==0,start=rng()%2;
  sim.state[sim.regs.at("replacementLFSR")]=lfsr;sim.state[sim.regs.at("s1_set_addr")]=set;sim.state[sim.regs.at("s1_tag_addr")]=tag;
  sim.memo.clear();sim.inputs={{"reset",reset},{"targetFire",fire},{"lookupStart",start},{"lookupSet",lookupSet},{"lookupTag",lookupTag},{"wayBits",ways},{"stage1Valid",valid},{"stage1IsWrite",write}};
  for(unsigned i=0;i<8;++i){
   auto s="read.data."+std::to_string(i)+".";sim.inputs[s+"tag"]=raw[i].tag;sim.inputs[s+"valid"]=raw[i].valid;sim.inputs[s+"dirty"]=raw[i].dirty;
   auto r="s1_metadata_"+std::to_string(i)+"_";
   sim.state[sim.regs.at(r+"tag")]=entries[i].tag;sim.state[sim.regs.at(r+"valid")]=entries[i].valid;sim.state[sim.regs.at(r+"dirty")]=entries[i].dirty;
  }
  unsigned enabled=1u<<ways,hit=8,free=8;
  for(unsigned i=0;i<enabled;++i){if(hit==8&&entries[i].valid&&entries[i].tag==tag)hit=i;if(free==8&&!entries[i].valid)free=i;}
  bool evict=hit==8&&free==8;unsigned victim=lfsr&(enabled-1),selected=hit<8?hit:free<8?free:victim;
  require(sim.get("hitValid")==unsigned(hit<8),"hit differs");require(sim.get("emptyValid")==unsigned(free<8),"empty differs");
  require(sim.get("evictDirty")==unsigned(evict&&entries[victim].dirty),"dirty eviction differs");require(sim.get("evictTag")==entries[victim].tag,"victim tag differs");
  require(sim.get("selectedWayOH")==unsigned(1u<<selected),"lowest-way/replacement selection differs");
  require(sim.get("read.en")==1,"asynchronous read must remain enabled");require(sim.get("read.addr")==lookupSet,"read address differs");
  require(sim.get("write.en")==unsigned(fire&&valid),"write enable reset/fire semantics differ");require(sim.get("write.addr")==set,"write address differs");
  for(unsigned i=0;i<8;++i){auto next=entries[i];if(i==selected){if(hit==8&&free<8)next.valid=true;if(write)next.dirty=true;else if(evict)next.dirty=false;if(hit==8)next.tag=uint32_t(tag);}
   auto prefix="write.data."+std::to_string(i)+".";require(sim.get(prefix+"tag")==next.tag,"tag update differs");require(sim.get(prefix+"valid")==next.valid,"valid update differs");require(sim.get(prefix+"dirty")==next.dirty,"dirty update differs");
   for(auto name:{"tag","valid","dirty"})require(sim.get("write.mask."+std::to_string(i)+"."+name)==1,"whole-way write mask differs");
  }
  unsigned nextLFSR=((lfsr>>1)|((((lfsr>>0)^(lfsr>>2)^(lfsr>>3)^(lfsr>>5))&1)<<15));
  sim.edge();require(sim.get("lfsr")==unsigned(fire?(reset?1:nextLFSR):lfsr),"LFSR seed/recurrence/fire/reset differs");
  for(unsigned i=0;i<8;++i){
   auto expected=fire&&start?raw[i]:entries[i];auto r="s1_metadata_"+std::to_string(i)+"_";
   require(sim.state.lookup(sim.regs.at(r+"tag"))==expected.tag,"metadata tag capture/hold differs");
   require(sim.state.lookup(sim.regs.at(r+"valid"))==expected.valid,"metadata valid capture/hold differs");
   require(sim.state.lookup(sim.regs.at(r+"dirty"))==expected.dirty,"metadata dirty capture/hold differs");
  }
  require(sim.get("stage1Set")==unsigned(fire?lookupSet:set),"stage1 set must sample every fire");require(sim.get("stage1Tag")==uint64_t(fire?lookupTag:tag),"stage1 tag must sample every fire");
  hits+=hit<8;empty+=free<8;evictions+=evict;dirtyEvictions+=evict&&entries[victim].dirty;resetWrites+=reset&&fire&&valid;stalls+=!fire;++cases;
 }
 require(hits&&empty&&evictions&&dirtyEvictions&&highTag&&resetWrites&&stalls,"metadata coverage incomplete");
 auto unchanged=print(root->getOperation());FModuleOp sentinel=top;require(failed(goldengate::materializeFASEDLLCMetadata(circuit,sentinel,error)),"duplicate accepted");require(sentinel==top&&unchanged==print(root->getOperation()),"duplicate rejection mutated IR/result");
 if(!output.empty()){circuit.setName(helper.getName());std::error_code ec;llvm::raw_fd_ostream os(output,ec);require(!ec,"cannot write fixture");root->print(os);os<<"\n";}
 llvm::outs()<<"FASED LLC metadata: "<<cases<<" vectors/transitions; "<<hits<<" hits; "<<empty<<" empty-way cases; "<<evictions<<" replacements; "<<dirtyEvictions<<" dirty replacements; "<<highTag<<" high-tag false-hit checks; "<<resetWrites<<" reset-time writes; "<<stalls<<" stalls\n";
}
}
int main(int argc,char **argv){try{require(argc<=2,"usage: goldengate-fased-llc-metadata-test [helper.mlir]");MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(ctx,argc==2?argv[1]:"");return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<"\n";return 1;}}
