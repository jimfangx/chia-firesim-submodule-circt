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
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5A5A5A5; }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 0; }
    require(regular == 64 && reset == 3, "wrong bank reset policy");
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
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<CatPrimOp>(op)) n = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) | eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = cast<UIntType>(v.getType()).getWidthOrSentinel();
    if (width < 64) n &= (uint64_t(1) << width) - 1;
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGLoadMemWriteMMIOWrapper" {
    firrtl.module @GGLoadMemWriteMMIOWrapper(
      in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
      out %loadmem_data_ready: !firrtl.uint<1>, in %loadmem_data_valid: !firrtl.uint<1>,
      in %loadmem_data_bits: !firrtl.uint<64>, out %other: !firrtl.uint<8>) {}
  } })",&context);
  require(bool(root),"fixture parse failed");auto c=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);SmallVector<Attribute> annos;
  for(auto n:{"loadmem_data_bits","other","hostReset"}) annos.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
    b.getNamedAttr("target",b.getStringAttr("~GGLoadMemWriteMMIOWrapper|GGLoadMemWriteMMIOWrapper>"+std::string(n)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annos));return root;
}
void behavior(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addLoadMemWriteData(c,error)),error);require(succeeded(verify(*root)),"data IR invalid");
  Interpreter sim(named(c,"GGLoadMemWriteDataFIFO"));std::deque<uint32_t> words;std::mt19937 random(171);
  unsigned cycles=0,pushes=0,pops=0,resetWrites=0,fullBlocks=0,partialBlocks=0;
  auto cycle=[&](bool reset,bool valid,bool ready) {
    ++cycles;sim.memo.clear();uint32_t data=random();
    sim.memo[sim.key(sim.arg(1))]=reset;sim.memo[sim.key(sim.arg(3))]=valid;
    sim.memo[sim.key(sim.arg(4))]=data;sim.memo[sim.key(sim.arg(5))]=ready;
    bool canPush=sim.eval(sim.arg(2)),canPop=sim.eval(sim.arg(6));
    require(canPush==(words.size()<64) && canPop==(words.size()>=2) && sim.eval(sim.arg(8))==words.size()/2,"occupancy/partial beat differs");
    if(canPop) require(sim.eval(sim.arg(7))==(uint64_t(words[0])|(uint64_t(words[1])<<32)),"packing/order differs");
    unsigned head=0;std::map<std::string,uint64_t> before;
    for(auto r:sim.module.getOps<RegResetOp>()) if(r.getName()=="head") head=sim.state.lookup(r.getResult());
    for(auto r:sim.module.getOps<RegOp>()) before[r.getName().str()]=sim.state.lookup(r.getResult());
    bool push=canPush&&valid,pop=canPop&&ready;
    if(pop){words.pop_front();words.pop_front();}if(push)words.push_back(data);if(reset)words.clear();
    sim.edge();sim.memo.clear();
    for(auto r:sim.module.getOps<RegOp>()) require(sim.state.lookup(r.getResult())==
        (push && r.getName()=="wdata_"+std::to_string(head) ? data : before.at(r.getName().str())),"reset payload/write selection differs");
    require(sim.eval(sim.arg(8))==words.size()/2,"next count differs");
    pushes+=push;pops+=pop;resetWrites+=reset&&push;fullBlocks+=!canPush&&valid&&ready;partialBlocks+=!canPop&&ready;
  };
  // Repeatedly fill beyond capacity, pop from full with a pending write, then
  // drain with an unmatched half-word. Wrap both pointers repeatedly.
  for(unsigned i=0;i<50;++i){cycle(true,true,false);for(unsigned j=0;j<66;++j)cycle(false,true,false);cycle(false,true,true);for(unsigned j=0;j<35;++j)cycle(false,false,true);cycle(false,true,true);cycle(false,false,true);}
  for(unsigned i=0;i<16000;++i)cycle(i%137==0,random()%4!=0,random()%4!=0);
  require(resetWrites&&fullBlocks&&partialBlocks&&pushes&&pops,"missing FIFO coverage");
  llvm::outs()<<"LoadMem write data: "<<cycles<<" cycles, "<<pushes<<" input words, "<<pops<<" output beats, "<<resetWrites<<" reset writes, "<<fullBlocks<<" full-with-pop blocks\n";
}
void mapping(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addLoadMemWriteData(c,error)),error);
  auto top=named(c,"GGLoadMemWriteDataWrapper"),fifo=named(c,"GGLoadMemWriteDataFIFO");
  require(top.getNumPorts()==4&&top.getPortName(3)=="loadmemData_mcr"&&fifo.getNumPorts()==9,"wrong data boundary");
  auto map=top->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");auto word=cast<DictionaryAttr>(map[0]);
  require(map.size()==1&&word.getAs<StringAttr>("name")=="W_DATA"&&word.getAs<IntegerAttr>("offset").getInt()==16&&
    !word.getAs<BoolAttr>("readable").getValue()&&word.getAs<BoolAttr>("writeable").getValue(),"wrong W_DATA register metadata");
  std::map<std::string,InstanceOp> inst;for(auto i:top.getOps<InstanceOp>())inst.emplace(i.getName().str(),i);
  auto data=inst.at("writeData"),inner=inst.at("sim");
  auto strict=[&](Value d,Value s){for(auto v:top.getOps<StrictConnectOp>())if(v.getDest()==d&&v.getSrc()==s)return true;return false;};
  require(strict(data.getResult(5),inner.getResult(2))&&strict(inner.getResult(3),data.getResult(6))&&strict(inner.getResult(4),data.getResult(7)),"writer binding differs");
  for(unsigned i=0;i<2;++i)require(strict(data.getResult(i),top.getBodyBlock()->getArgument(i)),"wrong host clock/reset");
  std::map<std::string,Value> drivers;Interpreter keys(fifo);
  for(auto v:top.getOps<StrictConnectOp>())drivers[keys.key(v.getDest())]=v.getSrc();
  auto mcr=keys.key(top.getBodyBlock()->getArgument(3));
  require(drivers.at(mcr+".write_4.ready")==data.getResult(2)&&
      keys.key(drivers.at(keys.key(data.getResult(3))))==mcr+".write_4.valid"&&
      keys.key(drivers.at(keys.key(data.getResult(4))))==mcr+".write_4.bits","decoded W_DATA connection differs");
  require(drivers.at(mcr+".read_4.valid").getDefiningOp<ConstantOp>().getValue().isZero()&&
      drivers.at(mcr+".read_4.bits").getDefiningOp<ConstantOp>().getValue().isZero(),"write-only read output differs");
  require(std::distance(top.getOps<AssertOp>().begin(),top.getOps<AssertOp>().end())==1,"missing read assertion");
  auto a=*top.getOps<AssertOp>().begin();auto pred=a->getOperand(1).getDefiningOp<NotPrimOp>(),enable=a->getOperand(2).getDefiningOp<NotPrimOp>();
  require(pred&&enable&&keys.key(pred->getOperand(0))==mcr+".read_4.ready"&&enable->getOperand(0)==top.getBodyBlock()->getArgument(1),"wrong access assertion/enable");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGLoadMemWriteDataWrapper|GGLoadMemWriteMMIOWrapper>loadmem_data_bits","~GGLoadMemWriteDataWrapper|GGLoadMemWriteDataWrapper>other","~GGLoadMemWriteDataWrapper|GGLoadMemWriteDataWrapper>hostReset"};
  for(unsigned i=0;i<3;++i)require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target")==targets[i],"target identity differs");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<10;++bad){
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGLoadMemWriteMMIOWrapper");OpBuilder b(&context);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad>=2&&bad<=4){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[bad]=b.getStringAttr("wrong");top.setPortNames(names);}
    if(bad==5||bad==6){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());names[5]=b.getStringAttr(bad==5?"loadmem_data_extra":"loadmemData_mcr");top.setPortNames(names);}
    if(bad==7){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==8){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGLoadMemWriteDataFIFO"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==9){SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[4]=TypeAttr::get(UIntType::get(&context,32,false));top.setPortTypes(types);}
    std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::addLoadMemWriteData(c,error)),"invalid data boundary accepted");{llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
}
}
int main(){try{MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(context);mapping(context);rejection(context);llvm::outs()<<"FIFO packing, reset retention, W_DATA mapping, permission and 10 atomic rejections passed\n";return 0;}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
