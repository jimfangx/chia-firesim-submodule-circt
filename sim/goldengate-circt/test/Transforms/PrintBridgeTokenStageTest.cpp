// See LICENSE for license details.
#include "goldengate/PrintBridgePayload.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok,const std::string &why) {if(!ok)throw std::runtime_error(why);}
std::string dump(Operation *op) {std::string text;llvm::raw_string_ostream out(text);op->print(out);return text;}
FModuleOp named(CircuitOp circuit,StringRef name) {
  for(auto module:circuit.getOps<FModuleOp>())if(module.getName()==name)return module;
  throw std::runtime_error("missing module: "+name.str());
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context,unsigned bits) {
  auto root=parseSourceString<ModuleOp>(
      "module { firrtl.circuit \"Top\" attributes {rawAnnotations = [{class = \"test.Opaque\", value = \"keep\"}]} {"
      "firrtl.module @Top() {} firrtl.module @GGPrintBridgeTokenStage() {}"
      "firrtl.module @Payload(in %hBits: !firrtl.bundle<reset: uint<1>>, out %valid: !firrtl.uint<1>, out %data: !firrtl.uint<"+
      std::to_string(bits)+">) {} }}",&context);
  require(bool(root),"token-stage fixture parse failed");
  auto circuit=*root->getOps<CircuitOp>().begin();OpBuilder b(&context);
  auto metadata=b.getDictionaryAttr({
      b.getNamedAttr("bridgeTarget",b.getStringAttr("~Top|Top>synthesizedPrintf")),
      b.getNamedAttr("resetPortName",b.getStringAttr("globalReset")),
      b.getNamedAttr("tokenBits",b.getI64IntegerAttr(bits)),
      b.getNamedAttr("tokenBytes",b.getI64IntegerAttr(bits/8)),
      b.getNamedAttr("idleCycleBits",b.getI64IntegerAttr(std::min(16u,bits)-1)),
      b.getNamedAttr("idleCycleMask",b.getI64IntegerAttr(((1u<<(std::min(16u,bits)-1))-1)<<1)),
      b.getNamedAttr("records",b.getArrayAttr({}))});
  named(circuit,"Payload")->setAttr("goldengate.printPayload",metadata);
  return root;
}
// Interpret the actual native register and expression graph. Register updates
// are simultaneous, including an unreset dataPipe storage register.
struct Interpreter {
  FModuleOp module;
  std::map<std::string,Value> drivers;
  std::map<std::string,APInt> memo;
  llvm::DenseMap<Value,APInt> state;
  std::map<std::string,Value> registers;
  std::string key(Value value) {return std::to_string(reinterpret_cast<uintptr_t>(value.getAsOpaquePointer()));}
  unsigned width(Value value) {return cast<UIntType>(value.getType()).getWidthOrSentinel();}
  Interpreter(FModuleOp m):module(m) {
    for(auto connect:m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(connect.getDest()),connect.getSrc()).second,"multiple token-stage drivers");
    m.walk([&](Operation *op) {
      if(!isa<RegOp,RegResetOp>(op))return;
      auto name=op->getAttrOfType<StringAttr>("name");
      require(name && registers.emplace(name.getValue().str(),op->getResult(0)).second,"duplicate state register name");
      unsigned w=width(op->getResult(0));state[op->getResult(0)]=APInt(w,0);
      if(auto reg=dyn_cast<RegResetOp>(op))
        require(reg.getClockVal()==arg(0) && reg.getResetSignal()==arg(1),"token state does not use host clock/reset");
      else require(cast<RegOp>(op).getClockVal()==arg(0),"queue storage does not use host clock");
    });
    require(registers.size()==3 && registers.count("dataPipe_full") &&
        registers.count("dataPipe_data") && registers.count("idleCycles"),"token stage state shape mismatch");
  }
  Value arg(unsigned index) {return module.getArgument(index);}
  APInt eval(Value value) {
    auto name=key(value);if(auto found=memo.find(name);found!=memo.end())return found->second;
    if(auto found=state.find(value);found!=state.end())return found->second;
    if(auto found=drivers.find(name);found!=drivers.end())return eval(found->second);
    auto *op=value.getDefiningOp();require(op!=nullptr,"missing token-stage input");
    unsigned w=width(value);APInt result(w,0);
    auto operand=[&](unsigned i){return eval(op->getOperand(i));};
    if(auto constant=dyn_cast<ConstantOp>(op))result=constant.getValue();
    else if(isa<AndPrimOp>(op))result=operand(0)&operand(1);
    else if(isa<OrPrimOp>(op))result=operand(0)|operand(1);
    else if(isa<XorPrimOp>(op))result=operand(0)^operand(1);
    else if(isa<NotPrimOp>(op))result=~operand(0);
    else if(isa<AndRPrimOp>(op))result=APInt(1,operand(0).isAllOnes());
    else if(isa<NEQPrimOp>(op))result=APInt(1,operand(0)!=operand(1));
    else if(isa<EQPrimOp>(op))result=APInt(1,operand(0)==operand(1));
    else if(isa<MuxPrimOp>(op))result=operand(operand(0).getBoolValue()?1:2);
    else if(isa<AddPrimOp>(op))result=operand(0).zextOrTrunc(w)+operand(1).zextOrTrunc(w);
    else if(isa<CatPrimOp>(op)) {
      auto a=operand(0),b=operand(1);result=a.zext(w).shl(b.getBitWidth())|b.zext(w);
    } else if(auto bits=dyn_cast<BitsPrimOp>(op))result=operand(0).lshr(bits.getLo()).trunc(w);
    else if(isa<PadPrimOp,NodeOp,AsUIntPrimOp>(op))result=operand(0).zextOrTrunc(w);
    else throw std::runtime_error("unsupported token-stage operation: "+op->getName().getStringRef().str());
    result=result.zextOrTrunc(w);memo.emplace(name,result);return result;
  }
  void edge() {
    llvm::DenseMap<Value,APInt> next;
    for(auto reg:module.getOps<RegResetOp>())
      next[reg.getResult()]=eval(reg.getResetSignal()).getBoolValue()?eval(reg.getResetValue()):eval(drivers.at(key(reg.getResult())));
    for(auto reg:module.getOps<RegOp>())next[reg.getResult()]=eval(drivers.at(key(reg.getResult())));
    state=std::move(next);
  }
};
struct Inputs {
  bool reset=false,done=true,valid=true,enable=true,flush=false,ready=true,print=false;
  APInt data;
  Inputs(unsigned bits):data(bits,0) {}
};
void behavior(MLIRContext &context,unsigned bits) {
  auto root=fixture(context,bits);auto circuit=*root->getOps<CircuitOp>().begin();auto payload=named(circuit,"Payload");
  auto raw=circuit->getAttr("rawAnnotations");auto layout=payload->getAttr("goldengate.printPayload");
  SmallVector<FModuleOp> stages;std::string error;
  require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit,{payload},stages,error)),error);
  require(stages.size()==1 && stages[0].getName()!="GGPrintBridgeTokenStage" && stages[0].isPublic() &&
      circuit->getAttr("rawAnnotations")==raw && payload->getAttr("goldengate.printPayload")==layout,
      "token materialization altered names, visibility, or existing collateral");
  auto stage=stages[0];auto metadata=stage->getAttrOfType<DictionaryAttr>("goldengate.printTokenStage");
  require(metadata && metadata.getAs<StringAttr>("payloadModule").getValue()=="Payload" &&
      metadata.getAs<IntegerAttr>("queueDepth").getInt()==1 &&
      metadata.getAs<BoolAttr>("pipe").getValue() && !metadata.getAs<BoolAttr>("flow").getValue(),
      "token staging metadata disagrees with Scala Queue(1,pipe=true,flow=false)");
  for(auto member:cast<DictionaryAttr>(layout))require(metadata.get(member.getName())==member.getValue(),"token stage lost payload collateral");
  const char *names[]={"hostClock","hostReset","doneInit","hValid","enable","flushNarrowPacket","bufferReady","payloadValid","payloadData","hReady","tokenValid","tokenData"};
  require(stage.getNumPorts()==12,"token stage port count differs");
  for(unsigned i=0;i<12;++i)require(stage.getPortName(i)==names[i] &&
      stage.getPortDirection(i)==(i<9?Direction::In:Direction::Out),"token stage interface identity mismatch");
  Interpreter sim(stage);std::optional<APInt> queue;APInt stored(bits,0);unsigned idle=0;
  unsigned mask=(1u<<(std::min(16u,bits)-1))-1;
  uint64_t cycles=0,simultaneous=0,stalls=0,rollovers=0,flushWithPending=0,outsideROI=0;
  auto cycle=[&](const Inputs &input) {
    ++cycles;sim.memo.clear();
    bool flags[]={input.reset,input.done,input.valid,input.enable,input.flush,input.ready,input.print};
    for(unsigned i=0;i<7;++i)sim.memo.emplace(sim.key(sim.arg(i+1)),APInt(1,flags[i]));
    sim.memo.emplace(sim.key(sim.arg(8)),input.data);
    bool targetAccepted=input.done&&input.valid&&!input.flush&&(!input.enable||input.ready);
    bool emitIdle=input.done&&input.valid&&!input.flush&&input.enable&&
        ((input.print&&idle!=0)||idle==mask);
    bool tokenValid=queue.has_value()||emitIdle;
    APInt token=queue?*queue:APInt(bits,idle).shl(1);
    require(sim.eval(sim.arg(9)).getBoolValue()==targetAccepted,"target acceptance differs from DecoupledHelper");
    require(sim.eval(sim.arg(10)).getBoolValue()==tokenValid,"token valid incorrectly gated by downstream readiness/control");
    require(sim.eval(sim.arg(11))==token,"queue data/idle encoding differs before edge");
    bool pop=queue.has_value()&&input.ready;
    bool push=targetAccepted&&input.enable&&input.print;
    if(push)require(!queue||input.ready,"target accepted print into blocked full queue");
    simultaneous+=pop&&push;stalls+=tokenValid&&!input.ready;
    rollovers+=emitIdle&&idle==mask&&targetAccepted;
    flushWithPending+=input.flush&&queue.has_value();outsideROI+=targetAccepted&&!input.enable;
    if(pop)queue.reset();if(push){queue=input.data;stored=input.data;}
    if(input.flush || push)idle=0;
    else if(targetAccepted&&input.enable)idle=idle==mask?1:idle+1;
    if(input.reset){queue.reset();idle=0;}
    sim.edge();
    require(sim.state.lookup(sim.registers.at("dataPipe_full"))==APInt(1,queue.has_value()),"queue occupancy update differs");
    require(sim.state.lookup(sim.registers.at("idleCycles")).getZExtValue()==idle,"idle counter update/flush/rollover differs");
    require(sim.state.lookup(sim.registers.at("dataPipe_data"))==stored,"unreset queue storage write/hold differs");
  };
  Inputs input(bits);input.reset=true;cycle(input);input.reset=false;
  for(unsigned i=0;i<3;++i)cycle(input);
  input.print=true;input.ready=false;input.data=APInt(bits,0xa5);cycle(input); // idle token holds under stall.
  input.ready=true;cycle(input); // idle token now; print buffered for next cycle.
  input.done=false;input.valid=false;input.enable=false;input.ready=false;input.print=false;cycle(input);
  input.flush=true;cycle(input); // flush clears idle, never the pending print.
  input.ready=true;cycle(input); // pending payload drains even during flush/outside ROI.
  input=Inputs(bits);input.flush=true;cycle(input);input.flush=false;input.print=true;
  for(unsigned i=0;i<20;++i){input.data=APInt(bits,i+0x31);cycle(input);} // full replacement emits old data.
  input.print=false;cycle(input); // drain final buffered print.
  input.reset=true;input.print=true;input.data=APInt(bits,0xdead);cycle(input); // reset wins occupancy but not RAM write.
  input=Inputs(bits);input.reset=true;cycle(input);input.reset=false;
  for(unsigned i=0;i<mask;++i)cycle(input); // reach rollover independently for every supported token width.
  input.ready=false;cycle(input);cycle(input);input.ready=true;cycle(input); // held max token then counter becomes1.
  input.enable=false;input.ready=false;cycle(input);input.print=true;cycle(input); // ROI-disabled advances regardless of buffer.
  std::mt19937_64 random(0x5ca1a+bits);
  for(unsigned sample=0;sample<5000;++sample) {
    input=Inputs(bits);input.reset=random()%61==0;input.done=random()%5!=0;input.valid=random()%4!=0;
    input.enable=random()%5!=0;input.flush=random()%17==0;input.ready=random()%3!=0;input.print=random()&1;
    input.data=APInt(bits,random());for(unsigned shift=64;shift<bits;shift+=64)input.data|=APInt(bits,random()).shl(shift);
    cycle(input);
  }
  require(simultaneous && stalls && rollovers && flushWithPending && outsideROI,"directed/random cycles missed a token-stage corner");
  require(succeeded(verify(*root)),"token-stage FIRRTL does not verify");
  auto before=dump(*root);auto count=stages.size();
  require(failed(goldengate::materializePrintBridgeTokenStages(circuit,{payload},stages,error)) &&
      dump(*root)==before && stages.size()==count,"repeated materialization duplicated state or collateral");
  llvm::outs()<<"Checked PrintBridge token width "<<bits<<" across "<<cycles<<" cycles\n";
}
void invalid(MLIRContext &context) {
  for(unsigned variant=0;variant<10;++variant) {
    auto root=fixture(context,16);auto circuit=*root->getOps<CircuitOp>().begin();auto payload=named(circuit,"Payload");OpBuilder b(&context);
    NamedAttrList layout(payload->getAttrOfType<DictionaryAttr>("goldengate.printPayload"));
    if(variant==0)payload->removeAttr("goldengate.printPayload");
    if(variant==1)layout.erase("bridgeTarget");
    if(variant==2)layout.set("resetPortName",b.getStringAttr(""));
    if(variant==3)layout.set("tokenBits",b.getI64IntegerAttr(7));
    if(variant==4)layout.set("tokenBits",b.getI64IntegerAttr(24));
    if(variant==5)layout.set("idleCycleBits",b.getI64IntegerAttr(7));
    if(variant==6)layout.set("tokenBits",b.getI64IntegerAttr(32));
    if(variant==7)layout.set("bridgeTarget",b.getStringAttr(""));
    if(variant==8)layout.set("tokenBits",b.getStringAttr("16"));
    if(variant==9)layout.erase("idleCycleBits");
    if(variant!=0)payload->setAttr("goldengate.printPayload",layout.getDictionary(&context));
    // A valid first payload must not be committed before this bad later entry.
    auto valid=fixture(context,16);auto validCircuit=*valid->getOps<CircuitOp>().begin();
    auto first=cast<FModuleOp>(named(validCircuit,"Payload")->clone());
    first.setName("EarlierPayload");circuit.getBodyBlock()->push_back(first);
    auto before=dump(*root);SmallVector<FModuleOp> stages{named(circuit,"Top")};std::string error;
    require(failed(goldengate::materializePrintBridgeTokenStages(circuit,{first,payload},stages,error)) &&
        !error.empty() && dump(*root)==before && stages.size()==1,"late malformed payload partially created token state");
  }
  auto root=fixture(context,8);auto circuit=*root->getOps<CircuitOp>().begin();auto payload=named(circuit,"Payload");
  auto foreignRoot=fixture(context,8);auto foreign=named(*foreignRoot->getOps<CircuitOp>().begin(),"Payload");
  for(auto list:{SmallVector<FModuleOp>{payload,payload},SmallVector<FModuleOp>{payload,foreign},SmallVector<FModuleOp>{payload,{}}}) {
    auto before=dump(*root),foreignBefore=dump(*foreignRoot);SmallVector<FModuleOp> stages;std::string error;
    require(failed(goldengate::materializePrintBridgeTokenStages(circuit,list,stages,error)) &&
        dump(*root)==before && dump(*foreignRoot)==foreignBefore && stages.empty(),"duplicate/foreign/null payload mutated either circuit");
  }
  auto before=dump(*root);SmallVector<FModuleOp> stages;std::string error;
  require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit,{},stages,error)) &&
      stages.empty() && dump(*root)==before,"empty payload list changed module graph");
}
void multiple(MLIRContext &context) {
  auto root=fixture(context,16);auto circuit=*root->getOps<CircuitOp>().begin();auto first=named(circuit,"Payload");
  auto second=cast<FModuleOp>(first->clone());second.setName("PayloadSecond");
  OpBuilder b(&context);NamedAttrList layout(second->getAttrOfType<DictionaryAttr>("goldengate.printPayload"));
  layout.set("resetPortName",b.getStringAttr("otherDomainReset"));
  second->setAttr("goldengate.printPayload",layout.getDictionary(&context));circuit.getBodyBlock()->push_back(second);
  auto raw=circuit->getAttr("rawAnnotations");SmallVector<FModuleOp> stages;std::string error;
  require(succeeded(goldengate::materializePrintBridgeTokenStages(circuit,{first,second},stages,error)),error);
  require(stages.size()==2 && stages[0].getName()!=stages[1].getName() && circuit->getAttr("rawAnnotations")==raw,
      "per-clock token stage identities merged or annotations changed");
  for(unsigned i=0;i<2;++i) {
    auto metadata=stages[i]->getAttrOfType<DictionaryAttr>("goldengate.printTokenStage");
    require(metadata.getAs<StringAttr>("bridgeTarget").getValue()=="~Top|Top>synthesizedPrintf" &&
        metadata.getAs<StringAttr>("payloadModule").getValue()==(i?"PayloadSecond":"Payload") &&
        metadata.getAs<StringAttr>("resetPortName").getValue()==(i?"otherDomainReset":"globalReset"),
        "per-clock token stage constructor association changed");
  }
  require(succeeded(verify(*root)),"multiple token stages contain invalid FIRRTL");
}
} // namespace
int main() {
  MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
  try {for(unsigned bits:{8u,16u,512u,1024u})behavior(context,bits);invalid(context);multiple(context);
    llvm::outs()<<"PrintBridge token stage PASS\n";return 0;}
  catch(const std::exception &error){llvm::errs()<<"PrintBridge token stage FAIL: "<<error.what()<<'\n';return 1;}
}
