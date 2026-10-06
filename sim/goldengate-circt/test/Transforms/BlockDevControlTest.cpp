// See LICENSE for license details.
#include "goldengate/ClockBridgeControl.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bankWords = 26) {
  std::string text = R"(
    module { firrtl.circuit "GGBlockDevMMIOWrapper" {
      firrtl.module @GGBlockDevMMIOWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %data: !firrtl.uint<8>,
        out %blockdevBridge_mcr: !firrtl.bundle<
          read: vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 26>,
          write flip: vector<bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>, 26>,
          wstrb flip: uint<4>>) {} } })";
  for (size_t pos = 0; (pos = text.find(", 26>", pos)) != std::string::npos;) {
    auto count = ", " + std::to_string(bankWords) + ">";
    text.replace(pos, 5, count); pos += count.size();
  }
  auto root=parseSourceString<ModuleOp>(text,&ctx);
  require(bool(root),"fixture parse failed");
  auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto str=[&](llvm::StringRef s){return b.getStringAttr(s);};
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
    b.getNamedAttr("class",str("test.BlockDevTransfer")),
    b.getNamedAttr("targets",b.getArrayAttr({str("~GGBlockDevMMIOWrapper"),
      str("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>hostReset"),
      str("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>data"),
      str("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper"),str("~Other|Other>data")})),
    b.getNamedAttr("nested",b.getDictionaryAttr({b.getNamedAttr("target",
      str("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>hostClock"))}))})}));
  auto inner = named(c,"GGBlockDevMMIOWrapper");
  b.setInsertionPointToEnd(c.getBodyBlock());
  const PortInfo ports[]{{str("mcr"),inner.getPortType(3),Direction::Out}};
  auto bank = b.create<FModuleOp>(c.getLoc(),str("GGBlockDevMMIOBank"),
      inner.getConventionAttr(),ports);
  SmallVector<Attribute> rows;
  // Offset order, rather than metadata row order, defines transport lanes.
  for (unsigned i = bankWords; i > 0; --i) rows.push_back(b.getDictionaryAttr({
    b.getNamedAttr("name",str("register_" + std::to_string(i - 1))),
    b.getNamedAttr("offset",b.getI64IntegerAttr(4 * (i - 1))),
    b.getNamedAttr("readable",b.getBoolAttr(true)),
    b.getNamedAttr("writeable",b.getBoolAttr(true))}));
  bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
  return root;
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
void behavior(MLIRContext &ctx, unsigned bankWords) {
  unsigned indexBits = llvm::Log2_64_Ceil(bankWords), indexMask = (1u << indexBits) - 1;
  auto root = fixture(ctx,bankWords); auto c = *root->getOps<CircuitOp>().begin();
  std::string error;
  require(succeeded(goldengate::mapBlockDevBridgeControl(c,25,12,error)),error);
  require(succeeded(verify(*root)),"BlockDev control invalid");
  auto adapter=named(c,"GGBlockDevMCRFile");
  require(std::distance(adapter.getOps<RegResetOp>().begin(),adapter.getOps<RegResetOp>().end())==4,"transaction flag count");
  require(std::distance(adapter.getOps<RegOp>().begin(),adapter.getOps<RegOp>().end())==(indexBits ? 5 : 3),"unreset payload capture count");
  for (auto r : adapter.getOps<RegOp>())
    if (r.getName()=="wIndex" || r.getName()=="rIndex")
      require(cast<UIntType>(r.getResult().getType()).getWidth()==indexBits,"BlockDev index width");
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
  std::mt19937_64 rng(201); unsigned readTransfers=0,writeTransfers=0,awFirst=0,wFirst=0;
  std::vector<bool> seenRead(indexMask+1), seenWrite(indexMask+1);
  for (unsigned cycle=0;cycle<24000;++cycle) {
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
  llvm::outs()<<bankWords<<"-word MCRFile: 24000 cycles, "<<writeTransfers<<" writes, "<<readTransfers<<" reads; reordered requests, reset and backpressure matched\n";
}

void mapping(MLIRContext &ctx) {
  auto root=fixture(ctx); auto c=*root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
  auto target=[&](llvm::StringRef s) { return b.getStringAttr(s); };
  c->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class",target("test.BlockDevTransfer")),
      b.getNamedAttr("targets",b.getArrayAttr({
          target("~GGBlockDevMMIOWrapper"),
          target("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>hostReset"),
          target("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>data"),
          target("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper"),
          target("~Other|Other>data")})),
      b.getNamedAttr("nested",b.getDictionaryAttr({b.getNamedAttr("target",
          target("~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>hostClock"))}))})}));
  std::string error;
  require(succeeded(goldengate::mapBlockDevBridgeControl(c,25,12,error)),error);
  require(succeeded(verify(*root)),"BlockDev control boundary invalid");
  auto top=named(c,"GGBlockDevBridgeControlWrapper"), adapter=named(c,"GGBlockDevMCRFile");
  require(top.getNumPorts()==4 && top.getPortName(3)=="blockdevBridge_ctrl" &&
      top.getPortDirection(3)==Direction::In,"BlockDev decoded bank was not replaced");
  InstanceOp sim,cr;
  for(auto i:top.getOps<InstanceOp>()) { if(i.getName()=="sim")sim=i; if(i.getName()=="crFile")cr=i; }
  require(sim && cr && sim.getModuleName()=="GGBlockDevMMIOWrapper" &&
      cr.getModuleName()=="GGBlockDevMCRFile","BlockDev control instance identities differ");
  Interpreter keys(adapter); std::map<std::string,std::string> wires;
  for(auto conn:top.getOps<ConnectOp>())wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc()));
  for(auto conn:top.getOps<StrictConnectOp>())wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc()));
  auto arg=[&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(wires.at(keys.key(cr.getResult(0)))==keys.key(arg(0)) &&
      wires.at(keys.key(cr.getResult(1)))==keys.key(arg(1)),"BlockDev transport clock/reset binding differs");
  require(wires.at(keys.key(sim.getResult(0)))==keys.key(arg(0)) &&
      wires.at(keys.key(sim.getResult(1)))==keys.key(arg(1)) &&
      wires.at(keys.key(arg(2)))==keys.key(sim.getResult(2)),"BlockDev copied ports differ");
  require(wires.at(keys.key(cr.getResult(2)))==keys.key(arg(3)) &&
      wires.at(keys.key(cr.getResult(3)))==keys.key(sim.getResult(3)),"BlockDev Nasti/MCR aggregate bindings differ");
  auto anno=cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  require(anno.getAs<StringAttr>("class").getValue()=="test.BlockDevTransfer","BlockDev annotation class changed");
  const llvm::StringRef expected[]{"~GGBlockDevBridgeControlWrapper",
      "~GGBlockDevBridgeControlWrapper|GGBlockDevBridgeControlWrapper>hostReset",
      "~GGBlockDevBridgeControlWrapper|GGBlockDevBridgeControlWrapper>data",
      "~GGBlockDevBridgeControlWrapper|GGBlockDevMMIOWrapper","~Other|Other>data"};
  auto targets=anno.getAs<ArrayAttr>("targets");
  require(targets.size()==5,"BlockDev target set changed");
  for(unsigned i=0;i<5;++i) require(cast<StringAttr>(targets[i]).getValue()==expected[i],"BlockDev target identity differs");
  require(anno.getAs<DictionaryAttr>("nested").getAs<StringAttr>("target").getValue()==
      "~GGBlockDevBridgeControlWrapper|GGBlockDevBridgeControlWrapper>hostClock","nested BlockDev target was not transferred");
  auto printed=[](ModuleOp m) { std::string s; llvm::raw_string_ostream out(s); m.print(out); return s; };
  auto mapped=printed(*root);
  require(failed(goldengate::mapBlockDevBridgeControl(c,25,12,error)),"repeated BlockDev control mapping accepted");
  require(mapped==printed(*root),"repeated BlockDev mapping mutated IR");
  for(unsigned bad=0;bad<30;++bad) {
    auto rejected=fixture(ctx,bad==25?33:26); auto circuit=*rejected->getOps<CircuitOp>().begin();
    auto inner=named(circuit,"GGBlockDevMMIOWrapper");
    if(bad==0)circuit->removeAttr("rawAnnotations");
    if(bad>=1 && bad<=3) {
      SmallVector<Attribute> types(inner.getPortTypes().begin(),inner.getPortTypes().end());
      types[bad==1?0:bad==2?1:3]=TypeAttr::get(UIntType::get(&ctx,2,false)); inner.setPortTypes(types);
    }
    if(bad==4 || bad==5) {
      SmallVector<Attribute> names(inner.getPortNames().begin(),inner.getPortNames().end());
      names[bad==4?3:2]=b.getStringAttr(bad==4?"missing":"blockdevBridge_ctrl"); inner.setPortNames(names);
    }
    if(bad==6) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(),inner,"used"); }
    if(bad==7 || bad==8) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      b.create<FModuleOp>(circuit.getLoc(),b.getStringAttr(bad==7?"GGBlockDevMCRFile":"GGBlockDevBridgeControlWrapper"),inner.getConventionAttr(),ArrayRef<PortInfo>{});
    }
    if(bad==9 || bad==10) circuit->setAttr("rawAnnotations",b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("class",b.getStringAttr("test")),
        b.getNamedAttr("nested",b.getDictionaryAttr({b.getNamedAttr("targets",b.getArrayAttr({
            b.getStringAttr(bad==9?"~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>blockdevBridge_mcr":
            "~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>blockdevBridge_mcr.read[25].bits")}))}))})}));
    if(bad==13)circuit.setNameAttr(b.getStringAttr("WrongTop"));
    auto bank=named(circuit,"GGBlockDevMMIOBank");
    if(bad==14)bank->removeAttr("goldengate.mmioRegisters");
    if(bad>=15 && bad<=26 && bad!=25) {
      auto rows=llvm::to_vector(bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"));
      if(bad==15)rows.clear();
      if(bad==16)rows[0]=b.getStringAttr("not a row");
      if((bad>=17 && bad<=23) || bad==26) {
        NamedAttrList row(cast<DictionaryAttr>(rows[0]));
        if(bad==17)row.set("offset",b.getI64IntegerAttr(-4));
        if(bad==18)row.set("offset",b.getI64IntegerAttr(101));
        if(bad==19)row.set("offset",b.getI64IntegerAttr(104));
        if(bad==20)row.set("name",b.getStringAttr("register_0"));
        if(bad==21)row.erase("writeable");
        if(bad==22)row.set("readable",b.getStringAttr("true"));
        if(bad==23)row.set("offset",b.getI64IntegerAttr(0));
        if(bad==26) { row.set("readable",b.getBoolAttr(false)); row.set("writeable",b.getBoolAttr(false)); }
        rows[0]=row.getDictionary(&ctx);
      }
      // A valid dense 25-word registry must reject stale 26-word bank lanes.
      if(bad==24)rows.erase(rows.begin());
      bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(rows));
    }
    if(bad==27)bank.erase();
    if(bad==28)bank.setPortTypes({TypeAttr::get(UIntType::get(&ctx,2,false))});
    if(bad==29)bank.setPortNames({b.getStringAttr("missing")});
    auto before=printed(*rejected);
    require(failed(goldengate::mapBlockDevBridgeControl(circuit,bad==11?6:bad==25?7:25,bad==12?0:12,error)),"invalid BlockDev boundary accepted");
    require(before==printed(*rejected),"BlockDev boundary rejection mutated IR");
  }
  llvm::outs()<<"BlockDev: aggregate control/bank bindings, six nested target transfers, 30 atomic boundary rejections and repeated-pass rejection passed\n";
}
}
int main() {
  try { MLIRContext ctx; ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    mapping(ctx);
    for (unsigned count : {1,26,32,33}) behavior(ctx,count);
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
  return 0;
}
