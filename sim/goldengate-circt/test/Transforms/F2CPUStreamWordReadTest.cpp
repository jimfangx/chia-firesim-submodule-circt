// See LICENSE for license details.
// Exercise emitted FIRRTL against DWORD and wider explicit-ACK PCI peeks.
#include "goldengate/CPUStreamRead.h"
#include "goldengate/ClockBridgeControl.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const std::string &why) { if (!value) throw std::runtime_error(why); }
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
struct Fixture {
  OwningOpRef<ModuleOp> root;
  CircuitOp circuit;
  FModuleOp engine;
  Fixture(MLIRContext &ctx, bool f2) {
    root = parseSourceString<ModuleOp>(R"(module {firrtl.circuit "Top" {
      firrtl.module @Top(in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %a: !firrtl.bundle<ready flip: uint<1>,valid: uint<1>,bits: uint<512>>,
        out %b: !firrtl.bundle<ready flip: uint<1>,valid: uint<1>,bits: uint<512>>,
        out %ac: !firrtl.uint<13>, out %bc: !firrtl.uint<13>) {}
    }})", &ctx);
    require(bool(root), "fixture parse failed");
    circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    circuit->setAttr("rawAnnotations", b.getArrayAttr({})); std::string error;
    require(succeeded(goldengate::addCPUStreamRead(circuit,
        {{"A", "a", 6144}, {"B", "b", 6144}}, error)), error);
    require(succeeded(goldengate::addCPUStreamCountBank(circuit,{{"A","ac",13},{"B","bc",13}},error)),error);
    require(succeeded(goldengate::mapCPUStreamControl(circuit,32,12,error)),error);
    if (f2) require(succeeded(goldengate::enableF2CPUStreamWordReads(circuit,error)),error);
    for(auto m:circuit.getOps<FModuleOp>()) if(m.getName()=="GGCPUStreamRead") engine=m;
    require(engine && succeeded(verify(*root)),"invalid generated engine");
    if(f2)require(succeeded(goldengate::validateF2CPUStreamWordReads(engine,error)),error);
  }
};
struct Sim {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> values, state;
  SmallVector<RegResetOp> registers;
  SmallVector<RegOp> plainRegisters;
  AssertOp assertion;
  Sim(FModuleOp m):module(m) {
    for(auto c:m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()),c.getSrc()).second,"duplicate driver");
    for(auto r:m.getOps<RegResetOp>()){registers.push_back(r);state.emplace(key(r.getResult()),APInt(9,0));}
    for(auto r:m.getOps<RegOp>()){plainRegisters.push_back(r);state.emplace(key(r.getResult()),APInt(*cast<UIntType>(r.getResult().getType()).getWidth(),0));}
    for(auto a:m.getOps<AssertOp>()) assertion=a;
  }
  std::string key(Value value) {
    if(auto field=value.getDefiningOp<SubfieldOp>()) return key(field.getInput())+"."+field.getFieldName().str();
    if(auto index=value.getDefiningOp<SubindexOp>()) return key(index.getInput())+"["+std::to_string(index.getIndex())+"]";
    return std::to_string(reinterpret_cast<uintptr_t>(value.getAsOpaquePointer()));
  }
  Value arg(unsigned i){return module.getArgument(i);}
  APInt eval(Value value) {
    std::string name=key(value);unsigned width=*cast<UIntType>(value.getType()).getWidth();
    if(values.count(name)) return values.at(name).zextOrTrunc(width);
    if(state.count(name)) return state.at(name).zextOrTrunc(width);
    Operation *op=value.getDefiningOp();APInt result(width,0);
    if(drivers.count(name)) result=eval(drivers.at(name));
    else if(auto c=dyn_cast_or_null<ConstantOp>(op)) result=c.getValue();
    else if(auto bits=dyn_cast_or_null<BitsPrimOp>(op)) result=eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(width);
    else if(isa_and_nonnull<PadPrimOp>(op)) result=eval(op->getOperand(0)).zextOrTrunc(width);
    else if(isa_and_nonnull<NotPrimOp>(op)) result=~eval(op->getOperand(0));
    else if(isa_and_nonnull<AndPrimOp>(op)) result=eval(op->getOperand(0))&eval(op->getOperand(1));
    else if(isa_and_nonnull<OrPrimOp>(op)) result=eval(op->getOperand(0))|eval(op->getOperand(1));
    else if(isa_and_nonnull<EQPrimOp>(op)) result=APInt(1,eval(op->getOperand(0))==eval(op->getOperand(1)));
    else if(isa_and_nonnull<MuxPrimOp>(op)) result=eval(op->getOperand(eval(op->getOperand(0)).isZero()?2:1));
    else if(isa_and_nonnull<AddPrimOp>(op)) result=eval(op->getOperand(0)).zextOrTrunc(width)+eval(op->getOperand(1)).zextOrTrunc(width);
    else throw std::runtime_error("unmodeled operation/input");
    values.insert_or_assign(name,result.zextOrTrunc(width));return result.zextOrTrunc(width);
  }
  void edge(){std::map<std::string,APInt> next;for(auto r:registers)
    next.emplace(key(r.getResult()),eval(r.getResetSignal()).isZero()?eval(drivers.at(key(r.getResult()))):eval(r.getResetValue()));for(auto r:plainRegisters)next.emplace(key(r.getResult()),eval(drivers.at(key(r.getResult()))));state=std::move(next);}
  void input(unsigned i,uint64_t n){values.insert_or_assign(key(arg(i)),APInt(64,n));}
  void source(unsigned i, bool valid, const APInt &data) {
    values.insert_or_assign(key(arg(2+i))+".valid",APInt(1,valid));
    values.insert_or_assign(key(arg(2+i))+".bits",data);
  }
  bool ready(unsigned i){return eval(drivers.at(key(arg(2+i))+".ready")).getBoolValue();}
  void transaction(unsigned stream,unsigned lane,unsigned size,bool rready,bool valid,bool reset=false,unsigned len=0,unsigned id=42) {
    values.clear();
    if(module.getNumPorts()==17)for(unsigned i=0;i<2;++i)values.insert_or_assign(key(arg(16))+"["+std::to_string(i)+"]",APInt(1,0));
    input(1,reset);input(5,valid);input(6,id);input(7,(uint64_t(stream)<<19)+lane*4);
    input(8,len);input(9,size);input(10,rready);
    for(unsigned i=0;i<2;++i){APInt bits(512,0);for(unsigned w=0;w<16;++w)bits|=APInt(512,0x10000000+i*0x10000+w).shl(32*w);source(i,true,bits);}
  }
};
void behavior(MLIRContext &ctx) {
  Fixture old(ctx,false),fixed(ctx,true);Sim a(old.engine),b(fixed.engine);
  unsigned oldPops=0,newPops=0;
  // Interleave two streams as the SDK reads each DWORD. Read-side stalls must
  // preserve the head; each stream may pop only after all sixteen word lanes.
  for(unsigned size:{2u,6u})for(unsigned word=0;word<16;++word)for(unsigned stream=0;stream<2;++stream)for(bool ready:{false,true}) {
    a.transaction(stream,word,size,ready,true,false,0,0x20);b.transaction(stream,word,size,ready,true,false,0,0x20);
    require(b.eval(b.assertion.getPredicate()).getBoolValue(),"legal F2 peek rejected");
    require(a.eval(a.assertion.getPredicate()).getBoolValue()==(size==6),"old size contract changed");
    for(unsigned i=0;i<2;++i){oldPops+=a.ready(i);newPops+=b.ready(i);
      require(b.ready(i)==(false),"DWORD consumed wrong source/head/lane");}
    require(a.eval(a.arg(13))==b.eval(b.arg(13)),"narrow valid changed");
    require(a.eval(a.arg(15))==b.eval(b.arg(15)),"word lanes were shifted instead of returning AXI lane data");
    a.edge();b.edge();
  }
  require(oldPops==64 && newPops==0,"regression did not expose sixteen-fold destructive-read bug");
  // Repeated aligned reads and arbitrary speculative requests retain both
  // heads. A software ACK, independent of AR/R activity and address, pops only
  // the selected stream. No transaction-count or final-lane assumption remains.
  for(unsigned stream=0;stream<2;++stream)for(unsigned extra=0;extra<35;++extra) {
    b.transaction(stream,0,6,true,true,false,0,0x20);
    require(!b.ready(0)&&!b.ready(1),"speculative/aligned PCI read consumed a head");
    b.transaction(stream,0,6,true,true,false,0,0x20);
    b.values.insert_or_assign(b.key(b.arg(16))+"["+std::to_string(stream)+"]",APInt(1,1));
    require(b.ready(stream)&&!b.ready(1-stream),"ACK consumed wrong or multiple streams");
  }
  // Supported 64-byte bursts remain exactly the old SFC operation graph's
  // behavior for every length, plus independent R/source stalls and reset.
  std::mt19937_64 random(20261104);
  for(unsigned len=0;len<256;++len)for(unsigned n=0;n<20;++n) {
    unsigned stream=random()%2,lane=random()%16;bool valid=random()&1,rready=random()&1,reset=random()%13==0;
    a.transaction(stream,lane,6,rready,valid,reset,len,n%4);b.transaction(stream,lane,6,rready,valid,reset,len,n%4);
    for(unsigned i=0;i<2;++i){bool present=random()%3!=0;APInt data(512,random());a.source(i,present,data);b.source(i,present,data);
      require(a.ready(i)==b.ready(i),"full-width queue handshake changed");}
    for(unsigned i:{4u,11u,12u,13u,14u,15u})require(a.eval(a.arg(i))==b.eval(b.arg(i)),"full-width AXI output changed");
    require(a.eval(a.assertion.getPredicate())==b.eval(b.assertion.getPredicate()),"full-width assertion changed");
    a.edge();b.edge();
  }
  for(unsigned size=0;size<8;++size)for(unsigned len:{0u,1u,255u}) {
    b.transaction(0,0,size,true,true,false,len,0x20);
    require(b.eval(b.assertion.getPredicate()).getBoolValue()==(size>=2&&size<=6&&len==0),"unsupported size/burst accepted");
  }
  // PCI physical widths must be naturally aligned and single-beat. The
  // shell-normalized ARSIZE6 preserves the legacy DWORD lane alignment.
  for (unsigned size=2;size<=6;++size)for (unsigned byte=0;byte<64;++byte)
  for (unsigned stream=0;stream<2;++stream)for (bool ready:{false,true}) {
    b.transaction(stream,0,size,ready,true,false,0,0x20);
    b.input(7,(uint64_t(stream)<<19)+byte);
    const unsigned alignment=size==6?4:(1u<<size);
    require(b.eval(b.assertion.getPredicate()).getBoolValue()==(byte%alignment==0),
            "PCI native width alignment/cross-line contract differs");
    require(!b.ready(0)&&!b.ready(1),"wide/speculative PCI peek consumed a head");
    APInt expected(512,0);
    for(unsigned word=0;word<16;++word)expected|=APInt(512,0x10000000+stream*0x10000+word).shl(32*word);
    require(b.eval(b.arg(13))==expected,"wide PCI read shifted or changed the selected head");
  }
  b.transaction(0,0,2,true,true,false,0,0x20);b.input(7,1);
  require(!b.eval(b.assertion.getPredicate()).getBoolValue(),"unaligned DWORD accepted");
  b.transaction(0,15,2,true,true,false,0,0x20);b.source(0,false,APInt(512,0));
  require(!b.eval(b.arg(11)).getBoolValue(),"empty selected queue returned valid data");
  b.transaction(0,0,2,true,true,false,0,0);
  require(!b.eval(b.assertion.getPredicate()).getBoolValue(),"narrow DMA-ID request accepted");
  auto before=dump(*fixed.root);std::string error;
  require(failed(goldengate::enableF2CPUStreamWordReads(fixed.circuit,error)) && dump(*fixed.root)==before,"repeat lowering was not atomic");
  require(!old.engine->hasAttr("goldengate.f2CPUWordReads") && fixed.engine->hasAttr("goldengate.f2CPUWordReads"),"F2 profile was implicit");
  llvm::outs()<<"PASS F2 PCI reads: old64/new0 native/normalized PCI dequeue regression, all4/8/16/32-byte natural alignments with explicit software ACK, interleaved lanes/stalls, all256full-widthburstlengths, reset/empty/invalidcontract\n";
}
void ackBank(MLIRContext &ctx) {
  Fixture fixture(ctx,true); FModuleOp bank;
  for(auto m:fixture.circuit.getOps<FModuleOp>())if(m.getName()=="GGCPUStreamCountBank")bank=m;
  require(bool(bank),"missing real ACK count bank");
  Sim sim(bank);
  for(unsigned source=0;source<2;++source)for(unsigned bits:{0u,1u,2u})for(unsigned strobe:{0u,7u,15u})
  for(bool reset:{false,true})for(bool nonempty:{false,true})for(bool valid:{false,true}) {
    sim.values.clear();sim.input(1,reset);sim.input(2,nonempty);sim.input(3,nonempty);
    auto mcr=sim.key(sim.arg(4));sim.values.insert_or_assign(mcr+".wstrb",APInt(4,strobe));
    for(unsigned i=0;i<2;++i){sim.values.insert_or_assign(mcr+".write["+std::to_string(i)+"].valid",APInt(1,valid&&i==source));
      sim.values.insert_or_assign(mcr+".write["+std::to_string(i)+"].bits",APInt(32,bits));}
    for(unsigned i=0;i<2;++i){std::string ack=sim.key(sim.arg(5))+"["+std::to_string(i)+"]";
      require(sim.eval(sim.drivers.at(ack)).getBoolValue()==(!reset&&valid&&i==source&&bits==1&&strobe==15&&nonempty),
              "count write ACK pulse ignored source/data/strobe/reset/empty");}
    for(auto assertion:bank.getOps<AssertOp>())if(sim.eval(assertion.getEnable()).getBoolValue())
      require(sim.eval(assertion.getPredicate()).getBoolValue()==(!valid||(bits==1&&strobe==15&&nonempty)||
          [&]{auto pred=assertion.getPredicate().getDefiningOp<OrPrimOp>();
              auto notv=pred.getLhs().getDefiningOp<NotPrimOp>();return sim.eval(notv.getInput()).isZero();}()),
          "ACK assertion does not reject malformed writes");
  }
  llvm::outs()<<"PASS actual CountBank ACK source/data/strobe/reset/empty Cartesian checks\n";
}
void axiACK(MLIRContext &ctx) {
  Fixture fixture(ctx,true);FModuleOp file,bank;
  for(auto m:fixture.circuit.getOps<FModuleOp>()){if(m.getName()=="GGCPUStreamMCRFile")file=m;if(m.getName()=="GGCPUStreamCountBank")bank=m;}
  Sim axi(file),counts(bank);unsigned observed=0;
  auto tick=[&](bool aw,bool w,unsigned source,unsigned data,unsigned strb,bool bready,bool reset){
    axi.values.clear();axi.input(1,reset);
    std::string bus=axi.key(axi.arg(2)),mcr=axi.key(axi.arg(3));
    auto put=[&](std::string name,unsigned width,uint64_t value){axi.values.insert_or_assign(bus+name,APInt(width,value));};
    put(".aw.valid",1,aw);put(".aw.bits.addr",32,664+4*source);put(".aw.bits.id",12,0);put(".aw.bits.len",8,0);
    put(".w.valid",1,w);put(".w.bits.data",32,data);put(".w.bits.strb",4,strb);put(".b.ready",1,bready);
    put(".ar.valid",1,0);put(".ar.bits.addr",32,664);put(".ar.bits.id",12,0);put(".ar.bits.len",8,0);put(".r.ready",1,0);
    counts.values.clear();counts.input(1,reset);counts.input(2,8);counts.input(3,8);
    auto cmcr=counts.key(counts.arg(4));
    unsigned held=axi.eval(axi.drivers.at(mcr+".wstrb")).getZExtValue();
    counts.values.insert_or_assign(cmcr+".wstrb",APInt(4,held));
    for(unsigned i=0;i<2;++i){
      std::string slot=".write["+std::to_string(i)+"]";
      axi.values.insert_or_assign(mcr+slot+".ready",APInt(1,1));
      axi.values.insert_or_assign(mcr+".read["+std::to_string(i)+"].valid",APInt(1,1));
      axi.values.insert_or_assign(mcr+".read["+std::to_string(i)+"].bits",APInt(32,8));
      counts.values.insert_or_assign(cmcr+slot+".valid",axi.eval(axi.drivers.at(mcr+slot+".valid")));
      counts.values.insert_or_assign(cmcr+slot+".bits",axi.eval(axi.drivers.at(mcr+slot+".bits")));
    }
    unsigned pulse=0;
    for(unsigned i=0;i<2;++i)if(counts.eval(counts.drivers.at(counts.key(counts.arg(5))+"["+std::to_string(i)+"]")).getBoolValue()){
      require(i==source,"real AXI source-count ACK selected the wrong source");++pulse;
    }
    require(pulse<=1,"one AXI write consumed multiple heads");observed+=pulse;
    bool bvalid=axi.eval(axi.drivers.at(bus+".b.valid")).getBoolValue();
    require(!pulse||!bvalid,"B response preceded ACK commit");
    axi.edge();return std::pair<unsigned,bool>{pulse,bvalid};
  };
  for(unsigned source=0;source<2;++source)for(unsigned strobe:{0u,7u,15u})for(unsigned data:{0u,1u,2u})for(unsigned order=0;order<3;++order){
    tick(false,false,source,0,0,false,true);observed=0;
    if(order==0){tick(true,false,source,data,strobe,false,false);tick(false,true,source,data,strobe,false,false);}
    if(order==1){tick(false,true,source,data,strobe,false,false);tick(true,false,source,99,0,false,false);}
    if(order==2)tick(true,true,source,data,strobe,false,false);
    for(unsigned stall=0;stall<5;++stall)tick(false,false,source,99,0,false,false);
    require(observed==unsigned(strobe==15&&data==1),"actual MCR W-data/strobe capture lost/duplicated ACK under AW/W order or B stalls");
    auto response=tick(false,false,source,0,0,true,false);require(response.second,"AXI write did not retire B");
    tick(false,false,source,0,0,false,false);
    require(observed==unsigned(strobe==15&&data==1),"B retirement duplicated ACK");
  }
  llvm::outs()<<"PASS actual CPU MCRFile AW-first/W-first/joint, held WDATA/WSTRB, B-stall single ACK, malformed data/strobe rejection\n";
}
void validation(MLIRContext &ctx) {
  for(unsigned bad=0;bad<9;++bad) {
    Fixture f(ctx,true);OpBuilder b(&ctx);auto loc=f.engine.getLoc();
    auto k=[&](unsigned width,uint64_t n)->Value{return b.create<ConstantOp>(loc,UIntType::get(&ctx,width),APInt(width,n));};
    if(bad==0)f.engine->setAttr("goldengate.f2CPUWordReads",b.getBoolAttr(true));
    if(bad==1||bad==2){NamedAttrList p(f.engine->getAttrOfType<DictionaryAttr>("goldengate.f2CPUWordReads"));
      p.set(bad==1?"pciRequestID":"unexpected",b.getI64IntegerAttr(0));
      f.engine->setAttr("goldengate.f2CPUWordReads",p.getDictionary(&ctx));}
    if(bad==3)for(auto eq:f.engine.getOps<EQPrimOp>())if(auto bits=eq.getLhs().getDefiningOp<BitsPrimOp>())
      if(bits.getHi()==1&&bits.getLo()==0){b.setInsertionPoint(eq);eq->setOperand(1,k(2,1));break;}
    if(bad==4||bad==5)for(auto c:f.engine.getOps<StrictConnectOp>()) {
      auto field=c.getDest().getDefiningOp<SubfieldOp>();
      if(field&&field.getFieldName()=="ready"){
        auto gate=c.getSrc().getDefiningOp<OrPrimOp>();require(bool(gate),"missing dequeue gate");
        b.setInsertionPoint(c);c->setOperand(1,bad==4?gate.getLhs():k(1,1));break;
      }
    }
    if(bad==6||bad==7)for(auto assertion:f.engine.getOps<AssertOp>()) {
      b.setInsertionPoint(assertion);assertion->setOperand(bad==6?1:2,k(1,bad==6));break;
    }
    if(bad==8)for(auto eq:f.engine.getOps<EQPrimOp>())
      if(eq.getLhs()==f.engine.getArgument(6)){b.setInsertionPoint(eq);eq->setOperand(1,k(16,0));break;}
    auto before=dump(*f.root);std::string error;
    require(failed(goldengate::validateF2CPUStreamWordReads(f.engine,error)) && dump(*f.root)==before,
            "header preflight accepted a spoofed DWORD contract or mutated input case "+std::to_string(bad));
  }
  for(unsigned bad=0;bad<9;++bad) {
    Fixture f(ctx,true); OpBuilder b(&ctx); FModuleOp bank,cw,file;
    for(auto m:f.circuit.getOps<FModuleOp>()){if(m.getName()=="GGCPUStreamCountBank")bank=m;if(m.getName()=="GGCPUStreamCountWrapper")cw=m;if(m.getName()=="GGCPUStreamMCRFile")file=m;}
    if(bad==0){auto attrs=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");SmallVector<Attribute> words(attrs.begin(),attrs.end());
      NamedAttrList word(cast<DictionaryAttr>(words[0]));word.set("writeable",b.getBoolAttr(false));words[0]=word.getDictionary(&ctx);
      bank->setAttr("goldengate.mmioRegisters",b.getArrayAttr(words));}
    if(bad==1)for(auto c:cw.getOps<StrictConnectOp>())if(auto inst=c.getDest().getDefiningOp<InstanceOp>())
      if(inst.getModuleName()=="GGCPUStreamReadWrapper"&&c.getDest()==inst.getResults().back()){c.erase();break;}
    if(bad==2||bad==3)for(auto c:bank.getOps<StrictConnectOp>())if(auto ix=c.getDest().getDefiningOp<SubindexOp>())
      if(ix.getInput()==bank.getBodyBlock()->getArguments().back()) {
        b.setInsertionPoint(c); Value v=bad==2?Value(b.create<ConstantOp>(bank.getLoc(),UIntType::get(&ctx,1),APInt(1,1))):
          Value(b.create<SubindexOp>(bank.getLoc(),bank.getBodyBlock()->getArguments().back(),1));c->setOperand(1,v);break;
      }
    if(bad==4){for(auto a:bank.getOps<AssertOp>()){a.erase();break;}}
    if(bad==5)for(auto c:file.getOps<StrictConnectOp>())if(auto field=c.getDest().getDefiningOp<SubfieldOp>())
      if(field.getFieldName()=="wstrb"&&field.getInput()==file.getArgument(3)){b.setInsertionPoint(c);c->setOperand(1,b.create<ConstantOp>(file.getLoc(),UIntType::get(&ctx,4),APInt(4,0)));break;}
    if(bad==6||bad==7)for(auto c:file.getOps<StrictConnectOp>())if(auto r=c.getDest().getDefiningOp<RegOp>())
      if(r->getAttrOfType<StringAttr>("name")=="f2AckWriteStrobe"){
        auto mux=c.getSrc().getDefiningOp<MuxPrimOp>();b.setInsertionPoint(c);
        mux->setOperand(bad==6?0:1,b.create<ConstantOp>(file.getLoc(),UIntType::get(&ctx,bad==6?1:4),APInt(bad==6?1:4,bad==6?1:15)));break;}
    if(bad==8)for(auto instance:cw.getOps<InstanceOp>())if(instance.getModuleName()=="GGCPUStreamCountBank") {
      b.setInsertionPointToEnd(cw.getBodyBlock());
      Value strobe=b.create<SubfieldOp>(cw.getLoc(),instance.getResult(4),"wstrb");
      b.create<StrictConnectOp>(cw.getLoc(),strobe,b.create<ConstantOp>(cw.getLoc(),UIntType::get(&ctx,4),APInt(4,0)));break;
    }
    std::string error;require(failed(goldengate::validateF2CPUStreamWordReads(f.engine,error)),
      "ACK route validator accepted permission/wiring/predicate/assertion mutation "+std::to_string(bad));
  }
  llvm::outs()<<"PASS F2 DWORD header preflight rejects nine metadata/alignment/ID/dequeue/assertion and nine ACK permission/wiring/predicate/assertion/AXI strobe mutations\n";
}
}
// Reproduce the NIC topology: control is attached outside several transparent
// wrappers rather than immediately beside GGCPUStreamCountWrapper.
void wrappedRoutes(MLIRContext &ctx) {
  for(unsigned bad=0;bad<9;++bad) {
    Fixture f(ctx,true);OpBuilder b(&ctx);auto loc=f.circuit.getLoc();
    InstanceOp count;
    f.circuit.walk([&](InstanceOp i){if(i.getModuleName()=="GGCPUStreamCountWrapper")count=i;});
    require(bool(count),"count instance missing");
    FModuleOp inner;
    for(auto m:f.circuit.getOps<FModuleOp>())if(m.getName()==count.getModuleName())inner=m;
    SmallVector<FModuleOp> wrappers;
    SmallVector<InstanceOp> children;
    InstanceOp current=count;
    for(unsigned depth=0;depth<3;++depth) {
      b.setInsertionPointToEnd(f.circuit.getBodyBlock());
      auto wrapper=b.create<FModuleOp>(loc,b.getStringAttr("NICRouteWrapper"+std::to_string(depth)),
        ConventionAttr::get(&ctx,Convention::Internal),inner.getPorts());
      b.setInsertionPoint(current);
      auto replacement=b.create<InstanceOp>(loc,wrapper,"nicRoute");
      for(auto [oldValue,newValue]:llvm::zip(current.getResults(),replacement.getResults()))oldValue.replaceAllUsesWith(newValue);
      current->moveBefore(wrapper.getBodyBlock(),wrapper.getBodyBlock()->end());
      b.setInsertionPointToEnd(wrapper.getBodyBlock());
      for(auto [i,port]:llvm::enumerate(inner.getPorts())) {
        Value a=wrapper.getArgument(i),c=current.getResult(i);
        b.create<ConnectOp>(loc,port.direction==Direction::In?c:a,
                                      port.direction==Direction::In?a:c);
      }
      wrappers.push_back(wrapper);children.push_back(current);
      inner=wrapper;current=replacement;
    }
    require(succeeded(verify(*f.root)),"invalid wrapper fixture");
    std::string error;
    if(bad==0) {
      require(succeeded(goldengate::validateF2CPUStreamWordReads(f.engine,error)),error);
      continue;
    }
    auto wrapper=wrappers[1];auto child=children[1];
    unsigned port=0;for(auto [i,p]:llvm::enumerate(wrapper.getPorts()))if(p.name=="cpuStream_mcr")port=i;
    b.setInsertionPointToEnd(wrapper.getBodyBlock());
    if(bad==1||bad==2) {
      Value bus=bad==1?wrapper.getArgument(port):child.getResult(port);
      auto strobe=b.create<SubfieldOp>(loc,bus,"wstrb");
      b.create<StrictConnectOp>(loc,strobe,b.create<ConstantOp>(loc,UIntType::get(&ctx,4),APInt(4,0)));
    }
    if(bad==3)b.create<ConnectOp>(loc,wrapper.getArgument(port),child.getResult(port));
    if(bad==4)for(auto c:llvm::make_early_inc_range(wrapper.getOps<ConnectOp>()))
      if(c.getDest()==wrapper.getArgument(port))c.erase();
    if(bad==5)b.create<InstanceOp>(loc,wrappers[0],"duplicateRoute");
    if(bad==6) {
      auto names=llvm::to_vector(wrapper.getPortNames());names[port]=b.getStringAttr("wrong_mcr");
      wrapper->setAttr("portNames",b.getArrayAttr(names));
    }
    if(bad==7) {
      auto conditional=b.create<WhenOp>(loc,b.create<ConstantOp>(loc,UIntType::get(&ctx,1),APInt(1,1)),false);
      b.setInsertionPointToStart(&conditional.getThenRegion().front());
      b.create<ConnectOp>(loc,wrapper.getArgument(port),child.getResult(port));
    }
    if(bad==8)b.create<ConnectOp>(loc,child.getResult(port),wrapper.getArgument(port));
    auto before=dump(*f.root);
    require(failed(goldengate::validateF2CPUStreamWordReads(f.engine,error))&&dump(*f.root)==before,
      "wrapped MCR route accepted override/duplicate/missing/ambiguous/port/conditional mutation "+std::to_string(bad));
  }
  llvm::outs()<<"PASS three-level NIC MCR route and eight intermediate-route negative controls\n";
}
int main(){try{MLIRContext ctx;ctx.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(ctx);ackBank(ctx);axiACK(ctx);validation(ctx);wrappedRoutes(ctx);return 0;}
catch(const std::exception &error){llvm::errs()<<error.what()<<'\n';return 1;}}
