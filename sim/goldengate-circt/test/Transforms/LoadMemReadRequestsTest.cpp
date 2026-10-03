// See LICENSE for license details.
#include "goldengate/LoadMemWriter.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
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
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
struct Interpreter {
  FModuleOp module;
  MemOp ram; std::array<uint64_t,2> memory{0x123456789,0xFEDCBA987};
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    ram=*m.getOps<MemOp>().begin();
    require(ram.getDepth()==2 && ram.getDataType()==UIntType::get(m.getContext(),34,false) &&
        ram.getReadLatency()==0 && ram.getWriteLatency()==1 && ram.getRuw()==RUWAttr::Undefined &&
        ram.getNumResults()==2 && ram.getPortKind(size_t(0))==MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1))==MemOp::PortKind::Write,"wrong queue RAM geometry");
    unsigned regular=0,reset=0;
    for(auto v:m.getOps<RegOp>()){++regular;state[v.getResult()]=3;require(v.getName()=="R_ADDRESS_H" && v.getResult().getType()==UIntType::get(m.getContext(),2,false),"wrong high address register");}
    for(auto v:m.getOps<RegResetOp>()){++reset;state[v.getResult()]=0;require(v.getResult().getType()==UIntType::get(m.getContext(),1,false),"wrong control width");}
    require(regular==1 && reset==3,"wrong reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<GTPrimOp>(op)) n = eval(op->getOperand(0)) > eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<CatPrimOp>(op)) n = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) | eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else if(auto f=dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput()==ram.getResult(0) && f.getFieldName()=="data","wrong RAM read");
      require(outputMemory(0,"en")==1,"RAM read disabled");n=memory.at(outputMemory(0,"addr"));
    }
    else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = cast<UIntType>(v.getType()).getWidthOrSentinel();
    if (width < 64) n &= (uint64_t(1) << width) - 1;
    memo[k] = n; return n;
  }
  uint64_t outputMemory(unsigned i,llvm::StringRef f) { return eval(drivers.at(key(ram.getResult(i))+"."+f.str())); }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    require(outputMemory(1,"mask")==1,"wrong RAM mask");
    if(outputMemory(1,"en"))memory.at(outputMemory(1,"addr"))=outputMemory(1,"data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGLoadMemWriteDataWrapper" {
    firrtl.module @GGLoadMemWriteDataWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })",&context);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("target",b.getStringAttr("~GGLoadMemWriteDataWrapper|GGLoadMemWriteDataWrapper>other"))})}));return root;
}
void behavior(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addLoadMemReadRequests(c,error)),error);require(succeeded(verify(*root)),"invalid read request IR");
  Interpreter sim(named(c,"GGLoadMemReadRequests"));std::deque<uint32_t> addresses;std::mt19937 random(172);
  unsigned high=3,cycles=0,pushes=0,pops=0,resetWrites=0,fullBlocks=0,emptyBlocks=0,highResetWrites=0;
  auto cycle=[&](bool reset,bool valid,bool ready,bool highValid) {
    ++cycles;sim.memo.clear();uint32_t low=random(),hi=random();
    for(auto [i,n]:std::map<unsigned,uint64_t>{{1,reset},{3,highValid},{4,hi},{8,valid},{9,low},{12,0},{13,ready}}) sim.memo[sim.key(sim.arg(i))]=n;
    bool canPush=sim.eval(sim.arg(7)),canPop=sim.eval(sim.arg(14));
    require(canPush==(addresses.size()<2) && canPop==!addresses.empty(),"read occupancy differs");
    require(sim.eval(sim.arg(2))==1 && sim.eval(sim.arg(5))==1 && sim.eval(sim.arg(6))==high &&
        sim.eval(sim.arg(10))==0 && sim.eval(sim.arg(11))==0,"read address permissions/readback differ");
    if(canPop)require(sim.eval(sim.arg(15))==addresses.front(),"read address packing/order differs");
    auto before=sim.memory;unsigned pos=sim.outputMemory(1,"addr");bool push=canPush&&valid,pop=canPop&&ready;
    if(pop)addresses.pop_front();if(push)addresses.push_back(low);if(reset)addresses.clear();if(highValid)high=hi&3;
    sim.edge();sim.memo.clear();
    for(unsigned i=0;i<2;++i)require(sim.memory[i]==(push&&i==pos ? low:before[i]),"reset-time RAM write differs");
    require(sim.eval(sim.arg(6))==high,"high address reset/capture differs");
    pushes+=push;pops+=pop;resetWrites+=reset&&push;highResetWrites+=reset&&highValid;
    fullBlocks+=!canPush&&valid&&ready;emptyBlocks+=!canPop&&valid&&ready;
  };
  for(unsigned i=0;i<100;++i){cycle(true,true,false,true);cycle(false,true,false,false);cycle(false,true,false,true);cycle(false,true,true,true);cycle(false,true,true,false);cycle(false,false,true,true);}
  for(unsigned i=0;i<20000;++i)cycle(i%23==0,random()&1,random()&1,random()&1);
  require(pushes>1000&&pops>1000&&resetWrites>100&&fullBlocks>100&&emptyBlocks>100&&highResetWrites>100,"missing queue coverage");
  llvm::outs()<<"LoadMem reads: "<<cycles<<" cycles, "<<pushes<<" pushes, "<<pops<<" pops, "<<resetWrites<<" reset RAM writes, "<<highResetWrites<<" high reset writes, "<<fullBlocks<<" full-with-pop blocks, "<<emptyBlocks<<" empty no-bypass cases\n";
}
void mapping(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addLoadMemReadRequests(c,error)),error);require(succeeded(verify(*root)),"invalid wrapper");
  auto top=named(c,"GGLoadMemReadRequestWrapper"),helper=named(c,"GGLoadMemReadRequests");Interpreter keys(helper);
  require(top.getNumPorts()==7&&top.getPortName(3)=="loadmemRead_mcr"&&helper.getNumPorts()==16,"wrong wrapper ports");
  auto metadata=top->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");require(metadata.size()==2,"missing MMIO metadata");
  for(unsigned i=0;i<2;++i){auto a=cast<DictionaryAttr>(metadata[i]);require(a.getAs<IntegerAttr>("offset").getInt()==24+4*i&&
      a.getAs<StringAttr>("name")== (i ? "R_ADDRESS_L":"R_ADDRESS_H") && a.getAs<BoolAttr>("readable").getValue()==(i==0) && a.getAs<BoolAttr>("writeable").getValue(),"wrong MMIO metadata");}
  auto anno=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);require(anno.getAs<StringAttr>("target")=="~GGLoadMemReadRequestWrapper|GGLoadMemReadRequestWrapper>other","wrong target transfer");
  InstanceOp req,sim;for(auto i:top.getOps<InstanceOp>()){if(i.getName()=="readRequests")req=i;else sim=i;}
  require(req&&sim,"missing wrapper instances");std::map<std::string,std::string> wires;
  for(auto v:top.getOps<StrictConnectOp>())wires[keys.key(v.getDest())]=keys.key(v.getSrc());
  auto outer=[&](unsigned i){return keys.key(top.getBodyBlock()->getArgument(i));};
  for(unsigned i=0;i<2;++i)require(wires.at(keys.key(req.getResult(i)))==outer(i),"wrong host clock/reset");
  for(unsigned word=6;word<=7;++word){unsigned start=word==6?2:7;std::string mcr=outer(3);
    require(wires.at(mcr+".write_"+std::to_string(word)+".ready")==keys.key(req.getResult(start)),"wrong write ready");
    for(unsigned i=1;i<=2;++i)require(wires.at(keys.key(req.getResult(start+i)))==mcr+".write_"+std::to_string(word)+(i==1?".valid":".bits"),"wrong write token");
    for(unsigned i=3;i<=4;++i)require(wires.at(mcr+".read_"+std::to_string(word)+(i==3?".valid":".bits"))==keys.key(req.getResult(start+i)),"wrong read token");
  }
  require(wires.at(keys.key(req.getResult(12)))==outer(3)+".read_7.ready"&&wires.at(keys.key(req.getResult(13)))==outer(4)&&
      wires.at(outer(5))==keys.key(req.getResult(14))&&wires.at(outer(6))==keys.key(req.getResult(15)),"wrong AR/assert binding");
  auto a=*helper.getOps<AssertOp>().begin();require(a->getOperand(1).getDefiningOp<NotPrimOp>()->getOperand(0)==helper.getBodyBlock()->getArgument(12)&&
      a->getOperand(2).getDefiningOp<NotPrimOp>()->getOperand(0)==helper.getBodyBlock()->getArgument(1),"wrong access assertion");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<10;++bad){auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGLoadMemWriteDataWrapper");OpBuilder b(&context);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2||bad==3){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[bad-2]=b.getStringAttr("wrong");top.setPortNames(names);}
    if(bad==4||bad==5){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[2]=b.getStringAttr(bad==4?"loadmemRead_mcr":"loadmem_mem_ar_extra");top.setPortNames(names);}
    if(bad==6){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==7||bad==8){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==7?"GGLoadMemReadRequests":"GGLoadMemReadRequestWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==9){SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[1]=TypeAttr::get(UIntType::get(&context,2,false));top.setPortTypes(types);}
    std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}require(failed(goldengate::addLoadMemReadRequests(c,error)),"invalid boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
}
}
int main(){try{MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(context);mapping(context);rejection(context);llvm::outs()<<"Read queue, high truncation/readback, reset writes, decoded mapping and ten atomic rejections passed\n";return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
