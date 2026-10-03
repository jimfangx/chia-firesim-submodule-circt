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
    require(regular == 1 && reset == 2, "wrong bank reset policy");
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
  auto root=parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGLoadMemReadRequestWrapper" {
    firrtl.module @GGLoadMemReadRequestWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })",&context);
  require(bool(root),"fixture parse failed"); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto n : {"hostClock","hostReset","other"}) annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class",b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target",b.getStringAttr("~GGLoadMemReadRequestWrapper|GGLoadMemReadRequestWrapper>"+std::string(n)))}));
  c->setAttr("rawAnnotations",b.getArrayAttr(annotations)); return root;
}
void behavior(MLIRContext &context) {
  auto root=fixture(context); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemReadData(c,error)),error);
  require(succeeded(verify(*root)),"read data IR invalid");
  Interpreter sim(named(c,"GGLoadMemReadDataFIFO")); std::deque<uint32_t> words; std::mt19937_64 random(173);
  unsigned cycles=0,pushes=0,pops=0,resetWrites=0,blockedLastPop=0,emptyBlocks=0;
  auto cycle=[&](bool reset,bool valid,bool ready) {
    ++cycles; sim.memo.clear(); uint64_t data=random();
    sim.memo[sim.key(sim.arg(1))]=reset; sim.memo[sim.key(sim.arg(3))]=valid;
    sim.memo[sim.key(sim.arg(4))]=data; sim.memo[sim.key(sim.arg(5))]=ready;
    bool canPush=sim.eval(sim.arg(2)),canPop=sim.eval(sim.arg(6));
    require(canPush==words.empty() && canPop==!words.empty() && sim.eval(sim.arg(8))==words.size(),"occupancy differs");
    if(canPop) require(sim.eval(sim.arg(7))==words.front(),"low-word-first output differs");
    Value payload=(*sim.module.getOps<RegOp>().begin()).getResult(); uint64_t old=sim.state.lookup(payload);
    bool push=canPush&&valid,pop=canPop&&ready;
    blockedLastPop+=words.size()==1&&valid&&ready; emptyBlocks+=words.empty()&&valid&&ready;
    if(pop) words.pop_front(); if(push) {words.push_back(uint32_t(data));words.push_back(uint32_t(data>>32));}
    if(reset) words.clear(); sim.edge(); sim.memo.clear();
    require(sim.state.lookup(payload)==(push?data:old),"reset payload write/retention differs");
    require(sim.eval(sim.arg(8))==words.size(),"next occupancy differs");
    pushes+=push;pops+=pop;resetWrites+=reset&&push;
  };
  for(unsigned i=0;i<100;++i) {
    cycle(true,true,true);cycle(false,true,true);cycle(false,true,false);
    cycle(false,true,true);cycle(false,true,true);cycle(false,false,true);
    cycle(false,true,false);cycle(true,false,true);
  }
  for(unsigned i=0;i<20000;++i)cycle(i%29==0,random()%4!=0,random()%4!=0);
  require(pushes&&pops&&resetWrites&&blockedLastPop&&emptyBlocks,"missing FIFO coverage");
  llvm::outs()<<"LoadMem read data: "<<cycles<<" cycles, "<<pushes<<" beats, "<<pops<<" words, "<<resetWrites
      <<" reset writes, "<<blockedLastPop<<" final-pop input blocks, "<<emptyBlocks<<" no-bypass cases\n";
}
void mapping(MLIRContext &context) {
  auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();std::string error;
  require(succeeded(goldengate::addLoadMemReadData(c,error)),error);
  auto top=named(c,"GGLoadMemReadDataWrapper"),fifo=named(c,"GGLoadMemReadDataFIFO");
  require(top.getNumPorts()==7&&top.getPortName(3)=="loadmemReadData_mcr"&&fifo.getNumPorts()==9,"wrong read boundary");
  auto metadata=top->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");auto word=cast<DictionaryAttr>(metadata[0]);
  require(metadata.size()==1&&word.getAs<StringAttr>("name")=="R_DATA"&&word.getAs<IntegerAttr>("offset").getInt()==32&&
      word.getAs<BoolAttr>("readable").getValue()&&!word.getAs<BoolAttr>("writeable").getValue(),"wrong R_DATA permissions");
  InstanceOp data;for(auto i:top.getOps<InstanceOp>())if(i.getName()=="readData")data=i;
  Interpreter keys(fifo);std::map<std::string,Value> drivers;
  for(auto conn:top.getOps<StrictConnectOp>())drivers.emplace(keys.key(conn.getDest()),conn.getSrc());
  auto a=[&](unsigned i){return top.getBodyBlock()->getArgument(i);};
  for(unsigned i=0;i<2;++i)require(drivers.at(keys.key(data.getResult(i)))==a(i),"wrong host binding");
  require(drivers.at(keys.key(a(4)))==data.getResult(2)&&drivers.at(keys.key(data.getResult(3)))==a(5)&&
      drivers.at(keys.key(data.getResult(4)))==a(6),"wrong memory R binding");
  auto rd=keys.key(a(3))+".read_8",wr=keys.key(a(3))+".write_8";
  require(keys.key(drivers.at(keys.key(data.getResult(5))))==rd+".ready"&&
      drivers.at(rd+".valid")==data.getResult(6)&&drivers.at(rd+".bits")==data.getResult(7)&&
      drivers.at(wr+".ready").getDefiningOp<ConstantOp>().getValue()==0,"wrong decoded R_DATA bindings");
  auto assertion=*top.getOps<AssertOp>().begin();
  require(assertion.getClock()==a(0)&&assertion.getMessage()=="Can only read from this decoupled source"&&
      keys.key(assertion.getPredicate().getDefiningOp<NotPrimOp>().getInput())==wr+".valid"&&
      assertion.getEnable().getDefiningOp<NotPrimOp>().getInput()==a(1),"wrong write assertion");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");require(annos.size()==3,"lost annotations");
  for(auto anno:annos)require(cast<DictionaryAttr>(anno).getAs<StringAttr>("target").getValue().starts_with(
      "~GGLoadMemReadDataWrapper|GGLoadMemReadDataWrapper>"),"copied target did not transfer");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<10;++bad) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGLoadMemReadRequestWrapper");OpBuilder b(&context);
    if(bad==0)c.setName("WrongTop");if(bad==1)c->removeAttr("rawAnnotations");
    if(bad==2||bad==3||bad==4||bad==5){SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end());
      names[bad==2?0:bad==3?1:2]=b.getStringAttr(bad==4?"loadmemReadData_mcr":bad==5?"loadmem_mem_r_extra":"wrong");top.setPortNames(names);}
    if(bad==6){b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad==7||bad==8){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(
      bad==7?"GGLoadMemReadDataFIFO":"GGLoadMemReadDataWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    if(bad==9){SmallVector<Attribute> types(top.getPortTypes().begin(),top.getPortTypes().end());types[1]=TypeAttr::get(UIntType::get(&context,2,false));top.setPortTypes(types);}
    std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::addLoadMemReadData(c,error)),"bad boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
}
}
int main(){try{MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(context);mapping(context);rejection(context);
  llvm::outs()<<"FIFO unpacking, reset writes, R_DATA permissions, targets and 10 atomic rejections passed\n";return 0;
}catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}}
