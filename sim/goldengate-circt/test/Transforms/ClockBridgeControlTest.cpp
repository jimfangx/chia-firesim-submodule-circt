// See LICENSE for license details.
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/TracerVTokenEngine.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <vector>
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef msg) { if (!ok) throw std::runtime_error(msg.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("module missing");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0,
                               unsigned bridge = 0, unsigned tsiWords = 9) {
  std::string top = bridge == 7 ? "GGTSIMMIOWrapper" : bridge == 6 ? "GGSimulationMasterWrapper" : bridge == 5 ? "GGLoadMemReadDataWrapper" : bridge == 4 ? "GGCPUStreamCountWrapper" : bridge == 1 ? "GGResetPulseBridgeWrapper" : bridge == 2 ? "GGPeekPokeMMIOWrapper" : bridge == 3 ? "GGTracerVTriggerWrapper" : "GGClockBridgeWrapper";
  std::string bank = bridge == 7 ? "tsiBridge_mcr" : bridge == 6 ? "simulationMaster_mcr" : bridge == 5 ? "loadmemWrite_mcr" : bridge == 4 ? "cpuStream_mcr" : bridge == 1 ? "resetBridge_mcr" : bridge == 2 ? "peekPokeBridge_mcr" : bridge == 3 ? "tracerv_mcr" : "clockBridge_mcr";
  unsigned words = bridge == 7 ? tsiWords : bridge == 6 ? 3 : bridge == 5 ? 9 : bridge == 4 ? 1 : bridge == 1 ? 2 : bridge == 2 ? 7 : bridge == 3 ? 15 : 6;
  std::string token="bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>";
  std::string header="module { firrtl.circuit \""+top+"\" { firrtl.module @"+top+"(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %data: !firrtl.uint<8>";
  if (bridge == 5) {
    const unsigned write[]{0,1,2,3,5}, data[]{4}, read[]{6,7}, readData[]{8};
    const std::pair<llvm::StringRef,ArrayRef<unsigned>> groups[]{{"loadmemWrite_mcr",write},{"loadmemData_mcr",data},
        {"loadmemRead_mcr",read},{"loadmemReadData_mcr",readData}};
    for (auto group : groups) {
      header+=", out %"+group.first.str()+": !firrtl.bundle<";
      for (auto word : group.second) header+="read_"+std::to_string(word)+": "+(bad==1&&word==0 ? "uint<31>" : token)+
          ", write_"+std::to_string(word)+" flip: "+token+", ";
      header+="wstrb flip: uint<4>>";
    }
  } else header+=", out %"+bank+": !firrtl.bundle<read: vector<"+token+", "+std::to_string(bad == 1 ? words - 1 : words)+">, write flip: vector<"+token+", "+std::to_string(words)+">, wstrb flip: uint<4>>";
  auto root=parseSourceString<ModuleOp>(header+") {} } }",&ctx);
  require(bool(root),"parse failed"); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto a=b.getDictionaryAttr({b.getNamedAttr("class",b.getStringAttr("test")),b.getNamedAttr("target",b.getStringAttr("~"+top+"|"+top+">"+(bad == 2 ? bank+".read[0].bits" : "data")))});
  c->setAttr("rawAnnotations",b.getArrayAttr({a}));
  if (bridge == 7) {
    auto inner = named(c,top);
    b.setInsertionPointToEnd(c.getBodyBlock());
    const PortInfo ports[]{{b.getStringAttr("mcr"),inner.getPortType(3),Direction::Out}};
    auto owner = b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGTSIMMIOBank"),
        inner.getConventionAttr(),ports);
    SmallVector<Attribute> rows;
    // Decode follows byte offsets, independently of metadata row order.
    for (unsigned i = words; i > 0; --i) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name",b.getStringAttr("register_"+std::to_string(i-1))),
      b.getNamedAttr("offset",b.getI64IntegerAttr(4*(i-1))),
      b.getNamedAttr("readable",b.getBoolAttr(true)),
      b.getNamedAttr("writeable",b.getBoolAttr(true))}));
    owner->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  }
  return root;
}
LogicalResult mapControl(CircuitOp c, unsigned bridge, unsigned addressBits,
                         unsigned idBits, std::string &error) {
  if (bridge == 7) return goldengate::mapTSIBridgeControl(c,addressBits,idBits,error);
  if (bridge == 6) return goldengate::mapSimulationMasterControl(c,addressBits,idBits,error);
  if (bridge == 5) return goldengate::mapLoadMemControl(c,addressBits,idBits,error);
  if (bridge == 4) return goldengate::mapCPUStreamControl(c,addressBits,idBits,error);
  if (bridge == 3) return goldengate::mapTracerVBridgeControl(c,addressBits,idBits,error);
  if (bridge == 2) return goldengate::mapPeekPokeBridgeControl(c,addressBits,idBits,error);
  return bridge == 1 ? goldengate::mapResetPulseBridgeControl(c,addressBits,idBits,error)
                     : goldengate::mapClockBridgeControl(c,addressBits,idBits,error);
}

struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto i = v.getDefiningOp<SubindexOp>()) return key(i.getInput()) + "[" + std::to_string(i.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp();
    uint64_t n;
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) {
      unsigned w = bits.getHi() - bits.getLo() + 1;
      n = (eval(bits.getInput()) >> bits.getLo()) & (w == 64 ? ~uint64_t(0) : (uint64_t(1) << w) - 1);
    } else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto a : module.getOps<AssertOp>())
      require(!eval(a.getEnable()) || eval(a.getPredicate()), "register permission assertion fired");
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx, unsigned bridge, unsigned tsiWords = 9) {
  unsigned bankWords = bridge == 7 ? tsiWords : bridge == 6 ? 3 : bridge == 5 ? 9 : bridge == 4 ? 1 : bridge == 1 ? 2 : bridge == 2 ? 7 : bridge == 3 ? 15 : 6;
  unsigned indexBits = llvm::Log2_64_Ceil(bankWords);
  unsigned indexMask = (1U << indexBits) - 1;
  std::string prefix = bridge == 7 ? "GGTSI" : bridge == 6 ? "GGSimulationMaster" : bridge == 5 ? "GGLoadMem" : bridge == 4 ? "GGCPUStream" : bridge == 1 ? "GGResetPulseBridge" : bridge == 2 ? "GGPeekPoke" : bridge == 3 ? "GGTracerV" : "GGClockBridge";
  auto root=fixture(ctx,0,bridge,tsiWords); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(mapControl(c,bridge,25,12,error)),error);
  require(succeeded(verify(*root)),"IR verification failed");
  auto adapter=named(c,prefix+"MCRFile"), wrapper=named(c,prefix+(bridge == 2 || bridge == 3 || bridge == 7 ? "BridgeControlWrapper" : "ControlWrapper"));
  require(wrapper.getPortName(3)==(bridge == 7 ? "tsiBridge_ctrl" : bridge == 6 ? "simulationMaster_ctrl" : bridge == 5 ? "loadmem_ctrl" : bridge == 4 ? "cpuStream_ctrl" : bridge == 1 ? "resetBridge_ctrl" : bridge == 2 ? "peekPokeBridge_ctrl" : bridge == 3 ? "tracerv_ctrl" : "clockBridge_ctrl") && wrapper.getPortDirection(3)==Direction::In,"slave boundary missing");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(cast<DictionaryAttr>(annos[0]).getAs<StringAttr>("target").getValue()=="~"+wrapper.getName().str()+"|"+wrapper.getName().str()+">data","copied target was not transferred");
  require(std::distance(adapter.getOps<RegResetOp>().begin(),adapter.getOps<RegResetOp>().end())==4,"transaction flag count");
  require(std::distance(adapter.getOps<RegOp>().begin(),adapter.getOps<RegOp>().end())==(indexBits ? 5 : 3),"unreset capture count");
  for (auto r : adapter.getOps<RegOp>())
    if (r.getName() == "wIndex" || r.getName() == "rIndex")
      require(cast<UIntType>(r.getResult().getType()).getWidth() == indexBits,
              "register index width disagrees with bank size");
  Interpreter sim(adapter); OpBuilder b(adapter.getBodyBlock(),adapter.getBodyBlock()->end()); auto loc=c.getLoc();
  std::map<std::pair<uintptr_t, std::string>, Value> fieldCache;
  auto field=[&](Value v,llvm::StringRef n)->Value{
    auto key = std::make_pair(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()), n.str());
    auto it = fieldCache.find(key);
    if (it != fieldCache.end()) return it->second;
    return fieldCache[key] = b.create<SubfieldOp>(loc,v,n);
  };
  auto nasti=[&](llvm::StringRef channel,llvm::StringRef member)->Value{return field(field(sim.arg(2),channel),member);};
  auto bits=[&](llvm::StringRef channel,llvm::StringRef member)->Value{return field(nasti(channel,"bits"),member);};
  std::map<std::pair<std::string, unsigned>, Value> slotCache;
  auto lane=[&](llvm::StringRef group,unsigned i,llvm::StringRef member)->Value{
    auto key = std::make_pair(group.str(), i);
    Value &slot = slotCache[key];
    if (!slot) slot=b.create<SubindexOp>(loc,field(sim.arg(3),group),i);
    return field(slot,member);
  };
  auto input=[&](Value v,uint64_t n){sim.memo[sim.key(v)]=n;};
  // Independent transaction reference from Scala MCRFile: accepted AW and W
  // queue separately; writes commit once, then B holds until consumption.
  bool haveAW=false,haveW=false,haveAR=false,done=false;
  uint64_t writeWord=0,readWord=0,writeID=0,readID=0,writeData=0;
  std::mt19937_64 rng(148); unsigned readTransfers=0,writeTransfers=0,awFirst=0,wFirst=0;
  std::vector<bool> seenRead(indexMask+1),seenWrite(indexMask+1);
  for (unsigned cycle=0;cycle<15000;++cycle) {
    sim.memo.clear(); bool reset=cycle<2 || cycle%137==0;
    bool awValid=rng()%3==0,wValid=rng()%3==0,arValid=rng()%3==0;
    bool rReady=cycle%211<40 ? false : bool(rng()&1);
    bool bReady=cycle%173<40 ? false : bool(rng()&1);
    uint64_t awAddr=rng()&0x1ffffff,arAddr=rng()&0x1ffffff;
    uint64_t awID=rng()&4095,arID=rng()&4095,wData=rng()&0xffffffff;
    input(sim.arg(1),reset);
    for (auto [channel,valid] : {std::pair<llvm::StringRef,bool>{"aw",awValid},{"w",wValid},{"ar",arValid}}) input(nasti(channel,"valid"),valid);
    input(nasti("r","ready"),rReady); input(nasti("b","ready"),bReady);
    input(bits("aw","addr"),awAddr);input(bits("ar","addr"),arAddr);
    input(bits("aw","id"),awID);input(bits("ar","id"),arID);
    input(bits("aw","len"),0);input(bits("ar","len"),0);input(bits("w","data"),wData);
    input(bits("w","strb"),rng()&15); input(bits("w","last"),rng()&1);
    std::vector<bool> readValid(bankWords),writeReady(bankWords);
    std::vector<uint64_t> readData(bankWords);
    bool wr=haveAW && haveW && !done;
    for (unsigned i=0;i<bankWords;++i) {
      readValid[i]=rng()%4!=0;writeReady[i]=rng()%4!=0;readData[i]=rng()&0xffffffff;
      input(lane("read",i,"valid"),readValid[i]);input(lane("read",i,"bits"),readData[i]);
      input(lane("write",i,"ready"),writeReady[i]);
      require(sim.eval(lane("write",i,"valid"))==(wr && writeWord==i),"selected write handshake mismatch");
      require(sim.eval(lane("write",i,"bits"))==writeData,"captured write data mismatch");
      require(sim.eval(lane("read",i,"ready"))==(haveAR && rReady && readWord==i),"selected read readiness mismatch");
    }
    unsigned select=readWord<bankWords ? readWord : 0;
    bool rv=haveAR && readValid[select],bv=haveAW && haveW && done;
    require(sim.eval(nasti("aw","ready"))==!haveAW && sim.eval(nasti("w","ready"))==!haveW && sim.eval(nasti("ar","ready"))==!haveAR,"request backpressure mismatch");
    require(sim.eval(nasti("r","valid"))==rv && sim.eval(nasti("b","valid"))==bv,"response validity mismatch");
    require(sim.eval(bits("r","data"))==readData[select] && sim.eval(bits("r","id"))==readID && sim.eval(bits("b","id"))==writeID,"response payload mismatch");
    require(sim.eval(bits("r","resp"))==0 && sim.eval(bits("b","resp"))==0 && sim.eval(bits("r","last"))==1 && sim.eval(bits("r","user"))==0 && sim.eval(bits("b","user"))==0,"response metadata mismatch");
    require(sim.eval(field(sim.arg(3),"wstrb"))==0,"legacy strobe handling mismatch");
    bool takeAW=!haveAW&&awValid,takeW=!haveW&&wValid,takeAR=!haveAR&&arValid;
    bool commit=wr && writeWord<bankWords && writeReady[writeWord];
    bool takeR=rv&&rReady,takeB=bv&&bReady;
    awFirst+=takeAW&&!haveW&&!takeW;wFirst+=takeW&&!haveAW&&!takeAW;
    readTransfers+=takeR;writeTransfers+=takeB;
    if(takeAW){writeWord=(awAddr>>2)&indexMask;writeID=awID;seenWrite[writeWord]=true;}
    if(takeW)writeData=wData;
    if(takeAR){readWord=(arAddr>>2)&indexMask;readID=arID;seenRead[readWord]=true;}
    if(takeAW)haveAW=true;if(takeW)haveW=true;if(takeAR)haveAR=true;
    if(takeR)haveAR=false;
    if(takeB){haveAW=false;haveW=false;done=false;}
    if(commit)done=true;
    if(reset){haveAW=false;haveW=false;haveAR=false;done=false;}
    sim.edge();
  }
  // Invalid indices intentionally stall writes until the next reset, limiting
  // completed transactions. Require both reorderings and substantial traffic.
  llvm::outs()<<bankWords<<"-word MCRFile coverage: reads="<<readTransfers<<", writes="<<writeTransfers
              <<", AW-first="<<awFirst<<", W-first="<<wFirst<<'\n';
  require(readTransfers>500 && writeTransfers>100 && awFirst>50 && wFirst>50,"insufficient transaction coverage");
  for(unsigned i=0;i<=indexMask;++i) require(seenRead[i] && seenWrite[i],"local index coverage incomplete");
  require(std::distance(adapter.getOps<AssertOp>().begin(),adapter.getOps<AssertOp>().end())==2,"AW/AR burst assertions missing");
  // Assertions must require an accepted address, and reset must mask them.
  for (auto a : adapter.getOps<AssertOp>()) {
    for (bool reset : {false,true}) for(bool accept : {false,true}) for(bool held : {false,true}) {
      sim.memo.clear(); input(sim.arg(1),reset);
      input(nasti("aw","valid"),accept);input(nasti("ar","valid"),accept);
      input(bits("aw","len"),7);input(bits("ar","len"),7);
      for(auto r:adapter.getOps<RegResetOp>())sim.state[r.getResult()]=held;
      require(sim.eval(a.getEnable())==(!reset&&accept&&!held),"burst assertion gating mismatch");
      require(sim.eval(a.getPredicate())==0,"burst assertion predicate mismatch");
    }
  }
  llvm::outs()<<bankWords<<"-word MCRFile: 15000 cycles, "<<writeTransfers<<" writes, "<<readTransfers<<" reads; reordered requests, reset and backpressure matched\n";
}

void countBank(MLIRContext &ctx) {
  for (unsigned bad = 0; bad < 6; ++bad) {
    std::string width = bad == 1 ? "12" : "13";
    std::string dir = bad == 2 ? "in" : "out";
    auto root = parseSourceString<ModuleOp>(
        "module { firrtl.circuit \"GGCPUStreamReadWrapper\" { firrtl.module @GGCPUStreamReadWrapper("
        "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, " + dir +
        " %tracerv_stream_count: !firrtl.uint<" + width + ">, out %data: !firrtl.uint<8>) {} } }", &ctx);
    require(bool(root), "count fixture parse");
    auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test")),
        b.getNamedAttr("targets", b.getArrayAttr({
          b.getStringAttr("~GGCPUStreamReadWrapper|GGCPUStreamReadWrapper>data"),
          b.getStringAttr("~GGCPUStreamReadWrapper|GGCPUStreamReadWrapper>tracerv_stream_count")}))})}));
    if (bad == 3) c->removeAttr("rawAnnotations");
    if (bad == 4) c.setName("WrongTop");
    if (bad == 5) {
      auto top = named(c, "GGCPUStreamReadWrapper");
      b.setInsertionPointToEnd(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "recursive");
    }
    std::string before, after, error;
    llvm::raw_string_ostream os(before); root->print(os); os.flush();
    auto result = goldengate::addCPUStreamCountBank(c, error);
    if (bad) {
      require(failed(result), "bad count boundary accepted");
      llvm::raw_string_ostream out(after); root->print(out); out.flush();
      require(before == after, "count rejection mutated input"); continue;
    }
    require(succeeded(result), error); require(succeeded(verify(*root)), "count bank invalid");
    auto bank = named(c, "GGCPUStreamCountBank");
    require(bank.getOps<RegOp>().empty() && bank.getOps<RegResetOp>().empty(), "count was captured");
    auto registers = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
    require(registers && registers.size() == 1, "count register map missing");
    auto reg = cast<DictionaryAttr>(registers[0]);
    require(reg.getAs<StringAttr>("name").getValue() == "TRACERVBRIDGEMODULE_0_to_cpu_stream_count" &&
        reg.getAs<IntegerAttr>("offset").getInt() == 0 && reg.getAs<BoolAttr>("readable").getValue() &&
        !reg.getAs<BoolAttr>("writeable").getValue(), "count register identity/access mismatch");
    auto targets = cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]).getAs<ArrayAttr>("targets");
    require(cast<StringAttr>(targets[0]).getValue() == "~GGCPUStreamCountWrapper|GGCPUStreamCountWrapper>data" &&
        cast<StringAttr>(targets[1]).getValue() == "~GGCPUStreamCountWrapper|GGCPUStreamReadWrapper>tracerv_stream_count", "count identity transfer mismatch");
    Interpreter sim(bank); b.setInsertionPointToEnd(bank.getBodyBlock());
    auto field = [&](Value v, llvm::StringRef n) -> Value { return b.create<SubfieldOp>(c.getLoc(), v, n); };
    auto lane = [&](llvm::StringRef group) -> Value { return b.create<SubindexOp>(c.getLoc(), field(sim.arg(3), group), 0); };
    Value rd = lane("read"), wr = lane("write"), data = field(rd, "bits"), valid = field(rd, "valid"), ready = field(wr, "ready"), write = field(wr, "valid");
    auto assertion = *bank.getOps<AssertOp>().begin();
    for (unsigned count = 0; count < 8192; ++count) for (unsigned flags = 0; flags < 4; ++flags) {
      sim.memo.clear(); sim.memo[sim.key(sim.arg(2))] = count;
      sim.memo[sim.key(sim.arg(1))] = flags & 1;
      sim.memo[sim.key(write)] = flags >> 1;
      require(sim.eval(data) == count && sim.eval(valid) == 1 && sim.eval(ready) == 1, "live count handshake mismatch");
      require(sim.eval(assertion.getEnable()) == !(flags & 1) &&
          sim.eval(assertion.getPredicate()) == !(flags >> 1), "read-only assertion mismatch");
    }
    require(succeeded(goldengate::mapCPUStreamControl(c, 25, 12, error)), error);
    require(succeeded(verify(*root)), "count/transport wiring invalid");
    llvm::outs() << "CPU stream count: 32768 live data/permission cases, five atomic rejections, annotation identity and transport wiring matched\n";
  }
}
void loadMemMapping(MLIRContext &ctx) {
  auto root=fixture(ctx,0,5); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::mapLoadMemControl(c,25,12,error)),error);
  require(succeeded(verify(*root)),"LoadMem control invalid");
  auto top=named(c,"GGLoadMemControlWrapper"); require(top.getNumPorts()==4,"sparse banks not consumed");
  Interpreter keys(named(c,"GGLoadMemMCRFile")); std::map<std::string,std::string> wires;
  InstanceOp sim,cr;
  for(auto i:top.getOps<InstanceOp>()) {if(i.getName()=="sim")sim=i;if(i.getName()=="crFile")cr=i;}
  for(auto conn:top.getOps<ConnectOp>())wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc()));
  const unsigned write[]{0,1,2,3,5}, data[]{4}, read[]{6,7}, readData[]{8};
  const ArrayRef<unsigned> groups[]{write,data,read,readData};
  unsigned mapped=0;
  for(unsigned j=0;j<4;++j) for(auto word:groups[j]) {
    auto lane=std::to_string(word);
    require(wires.at(keys.key(cr.getResult(3))+".read["+lane+"]")==keys.key(sim.getResult(3+j))+".read_"+lane,
        "read lane routed to wrong sparse bank");
    require(wires.at(keys.key(sim.getResult(3+j))+".write_"+lane)==keys.key(cr.getResult(3))+".write["+lane+"]",
        "write lane routed to wrong sparse bank"); ++mapped;
  }
  require(mapped==9,"not all nine MMIO words mapped");
  for(unsigned bad=0;bad<11;++bad) {
    auto root=fixture(ctx,0,5);auto c=*root->getOps<CircuitOp>().begin();auto inner=named(c,"GGLoadMemReadDataWrapper");OpBuilder b(&ctx);
    if(bad<4)c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr("test")),b.getNamedAttr("target",b.getStringAttr(
        "~GGLoadMemReadDataWrapper|GGLoadMemReadDataWrapper>"+inner.getPortName(3+bad).str()+".wstrb"))})}));
    if(bad==4)c->removeAttr("rawAnnotations");
    if(bad==5||bad==6) {SmallVector<Attribute> types(inner.getPortTypes().begin(),inner.getPortTypes().end());
      types[bad==5?1:3]=TypeAttr::get(UIntType::get(&ctx,2,false));inner.setPortTypes(types);}
    if(bad==7||bad==8){SmallVector<Attribute> names(inner.getPortNames().begin(),inner.getPortNames().end());
      names[6]=b.getStringAttr(bad==7?"missing":"loadmem_ctrl");inner.setPortNames(names);}
    if(bad==9){b.setInsertionPointToStart(inner.getBodyBlock());b.create<InstanceOp>(c.getLoc(),inner,"used");}
    if(bad==10){b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGLoadMemMCRFile"),inner.getConventionAttr(),ArrayRef<PortInfo>{});}
    std::string before,after,error;{llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::mapLoadMemControl(c,25,12,error)),"invalid sparse bank accepted");
    {llvm::raw_string_ostream out(after);root->print(out);} require(before==after,"sparse bank rejection mutated IR");
  }
  llvm::outs()<<"LoadMem: all nine sparse bank read/write bindings and 11 atomic rejections passed\n";
}
void tsiMapping(MLIRContext &ctx) {
  auto root=fixture(ctx,0,7); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto target=[&](llvm::StringRef s) { return b.getStringAttr(s); };
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class",target("test.TSITransfer")),
      b.getNamedAttr("targets",b.getArrayAttr({
          target("~GGTSIMMIOWrapper"),
          target("~GGTSIMMIOWrapper|GGTSIMMIOWrapper>hostReset"),
          target("~GGTSIMMIOWrapper|GGTSIMMIOWrapper>data"),
          target("~GGTSIMMIOWrapper|GGTSIMMIOWrapper"),
          target("~Other|Other>data")})),
      b.getNamedAttr("nested",b.getDictionaryAttr({b.getNamedAttr("target",
          target("~GGTSIMMIOWrapper|GGTSIMMIOWrapper>hostClock"))}))})}));
  std::string error;
  require(succeeded(goldengate::mapTSIBridgeControl(c,25,12,error)),error);
  require(succeeded(verify(*root)),"TSI control boundary invalid");
  auto top=named(c,"GGTSIBridgeControlWrapper"), adapter=named(c,"GGTSIMCRFile");
  require(top.getNumPorts()==4 && top.getPortName(3)=="tsiBridge_ctrl" &&
      top.getPortDirection(3)==Direction::In,"TSI decoded bank was not replaced");
  InstanceOp sim,cr;
  for(auto i:top.getOps<InstanceOp>()) { if(i.getName()=="sim")sim=i; if(i.getName()=="crFile")cr=i; }
  require(sim && cr && sim.getModuleName()=="GGTSIMMIOWrapper" &&
      cr.getModuleName()=="GGTSIMCRFile","TSI control instance identities differ");
  Interpreter keys(adapter); std::map<std::string,std::string> wires;
  for(auto conn:top.getOps<ConnectOp>())wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc()));
  for(auto conn:top.getOps<StrictConnectOp>())wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc()));
  auto arg=[&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(wires.at(keys.key(cr.getResult(0)))==keys.key(arg(0)) &&
      wires.at(keys.key(cr.getResult(1)))==keys.key(arg(1)),"TSI transport clock/reset binding differs");
  require(wires.at(keys.key(sim.getResult(0)))==keys.key(arg(0)) &&
      wires.at(keys.key(sim.getResult(1)))==keys.key(arg(1)) &&
      wires.at(keys.key(arg(2)))==keys.key(sim.getResult(2)),"TSI copied ports differ");
  require(wires.at(keys.key(cr.getResult(2)))==keys.key(arg(3)) &&
      wires.at(keys.key(cr.getResult(3)))==keys.key(sim.getResult(3)),"TSI Nasti/MCR aggregate bindings differ");
  auto anno=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  require(anno.getAs<StringAttr>("class").getValue()=="test.TSITransfer","TSI annotation class changed");
  const llvm::StringRef expected[]{"~GGTSIBridgeControlWrapper",
      "~GGTSIBridgeControlWrapper|GGTSIBridgeControlWrapper>hostReset",
      "~GGTSIBridgeControlWrapper|GGTSIBridgeControlWrapper>data",
      "~GGTSIBridgeControlWrapper|GGTSIMMIOWrapper","~Other|Other>data"};
  auto targets=anno.getAs<ArrayAttr>("targets");
  require(targets.size()==5,"TSI target set changed");
  for(unsigned i=0;i<5;++i) require(cast<StringAttr>(targets[i]).getValue()==expected[i],"TSI target identity differs");
  require(anno.getAs<DictionaryAttr>("nested").getAs<StringAttr>("target").getValue()==
      "~GGTSIBridgeControlWrapper|GGTSIBridgeControlWrapper>hostClock","nested TSI target was not transferred");
  auto printed=[](ModuleOp m) { std::string s; llvm::raw_string_ostream out(s); m.print(out); return s; };
  auto mapped=printed(*root);
  require(failed(goldengate::mapTSIBridgeControl(c,25,12,error)),"repeated TSI control mapping accepted");
  require(mapped==printed(*root),"repeated TSI mapping mutated IR");
  for(unsigned bad=0;bad<30;++bad) {
    auto rejected=fixture(ctx,0,7,bad==25?17:9); auto circuit=*rejected->getOps<CircuitOp>().begin();
    auto inner=named(circuit,"GGTSIMMIOWrapper");
    if(bad==0)circuit->removeAttr("rawAnnotations");
    if(bad>=1 && bad<=3) {
      SmallVector<Attribute> types(inner.getPortTypes().begin(),inner.getPortTypes().end());
      types[bad==1?0:bad==2?1:3]=TypeAttr::get(UIntType::get(&ctx,2,false)); inner.setPortTypes(types);
    }
    if(bad==4 || bad==5) {
      SmallVector<Attribute> names(inner.getPortNames().begin(),inner.getPortNames().end());
      names[bad==4?3:2]=b.getStringAttr(bad==4?"missing":"tsiBridge_ctrl"); inner.setPortNames(names);
    }
    if(bad==6) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(),inner,"used"); }
    if(bad==7 || bad==8) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      b.create<FModuleOp>(circuit.getLoc(),b.getStringAttr(bad==7?"GGTSIMCRFile":"GGTSIBridgeControlWrapper"),inner.getConventionAttr(),ArrayRef<PortInfo>{});
    }
    if(bad==9 || bad==10) circuit->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr("test")),
        b.getNamedAttr("nested",b.getDictionaryAttr({b.getNamedAttr("targets",b.getArrayAttr({
            b.getStringAttr(bad==9?"~GGTSIMMIOWrapper|GGTSIMMIOWrapper>tsiBridge_mcr":
            "~GGTSIMMIOWrapper|GGTSIMMIOWrapper>tsiBridge_mcr.read[8].bits")}))}))})}));
    if(bad==13)circuit.setNameAttr(b.getStringAttr("WrongTop"));
    auto bank=named(circuit,"GGTSIMMIOBank");
    if(bad==14)bank->removeAttr("goldengate.mmioRegisters");
    if(bad>=15 && bad<=26 && bad!=25) {
      auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      if(bad==15)rows.clear();
      if(bad==16)rows[0]=b.getStringAttr("not a row");
      if((bad>=17 && bad<=23) || bad==26) {
        NamedAttrList row(cast<DictionaryAttr>(rows[0]));
        if(bad==17)row.set("offset",b.getI64IntegerAttr(-4));
        if(bad==18)row.set("offset",b.getI64IntegerAttr(33));
        if(bad==19)row.set("offset",b.getI64IntegerAttr(36));
        if(bad==20)row.set("name",b.getStringAttr("register_0"));
        if(bad==21)row.erase("writeable");
        if(bad==22)row.set("readable",b.getStringAttr("true"));
        if(bad==23)row.set("offset",b.getI64IntegerAttr(0));
        if(bad==26) { row.set("readable",b.getBoolAttr(false)); row.set("writeable",b.getBoolAttr(false)); }
        rows[0]=row.getDictionary(&ctx);
      }
      // A valid dense eight-word registry must reject stale nine-word bank lanes.
      if(bad==24)rows.erase(rows.begin());
      bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if(bad==27)bank.erase();
    if(bad==28)bank.setPortTypes({TypeAttr::get(UIntType::get(&ctx,2,false))});
    if(bad==29)bank.setPortNames({b.getStringAttr("missing")});
    auto before=printed(*rejected);
    require(failed(goldengate::mapTSIBridgeControl(circuit,bad==11?5:bad==25?6:25,bad==12?0:12,error)),"invalid TSI boundary accepted");
    require(before==printed(*rejected),"TSI boundary rejection mutated IR");
  }
  llvm::outs()<<"TSI: aggregate control/bank bindings, six nested target transfers, 30 atomic boundary rejections and repeated-pass rejection passed\n";
}
void rejection(MLIRContext &ctx, unsigned bridge) {
  for(unsigned bad=0;bad<5;++bad){
    auto root=fixture(ctx,bad<3 ? bad : 0,bridge);auto c=*root->getOps<CircuitOp>().begin();
    std::string before,after,error;llvm::raw_string_ostream os(before);root->print(os);os.flush();
    unsigned addr=bad==0 ? (bridge == 4 ? 1 : bridge == 1 ? 2 : bridge == 6 ? 3 : 4) : 25,id=bad==3 ? 0 : 12;
    if(bad==4)c.setNameAttr(StringAttr::get(&ctx,"WrongTop"));
    if(bad==4){before.clear();llvm::raw_string_ostream changed(before);root->print(changed);}
    require(failed(mapControl(c,bridge,addr,id,error)),"bad input accepted");
    llvm::raw_string_ostream as(after);root->print(as);as.flush();
    require(before==after,"rejected mapping mutated input");
  }
}
}
int main(){
  try {MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();countBank(ctx);loadMemMapping(ctx);tsiMapping(ctx);for(unsigned bridge : {0U,1U,2U,3U,4U,5U,6U,7U}) {behavior(ctx,bridge);rejection(ctx,bridge);}
    for(unsigned count : {1U,16U,17U}) behavior(ctx,7,count);}
  catch(const std::exception &e){llvm::errs()<<e.what()<<'\n';return 1;}return 0;
}
