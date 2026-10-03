// See LICENSE for license details.
#include "goldengate/BlockDevMMIOBank.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
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
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5 & ((uint64_t(1) << cast<UIntType>(r.getResult().getType()).getWidthOrSentinel()) - 1); }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 20 && reset == 6, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0)) & 1;
    else if (isa_and_nonnull<CatPrimOp>(op)) n = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) | eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
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
constexpr unsigned widths[]{24,24,32,32,1,1,32,32,1,1,1,32,32,1,1,32,32,1,1,1,1,1,1,1,1,1};
const llvm::StringRef regNames[]{"read_latency", "write_latency", "bdev_nsectors", "bdev_max_req_len",
 "bdev_req_valid", "bdev_req_write", "bdev_req_offset", "bdev_req_len", "bdev_req_tag", "bdev_req_ready",
 "bdev_data_valid", "bdev_data_data_upper", "bdev_data_data_lower", "bdev_data_tag", "bdev_data_ready",
 "bdev_rresp_data_upper", "bdev_rresp_data_lower", "bdev_rresp_tag", "bdev_rresp_valid", "bdev_rresp_ready",
 "bdev_wack_tag", "bdev_wack_valid", "bdev_wack_ready", "bdev_reqs_pending", "bdev_wack_stalled", "bdev_rresp_stalled"};
bool pulse(unsigned i) { return i==9 || i==14 || i==18 || i==21; }
uint32_t mask(unsigned i) { return widths[i]==32 ? UINT32_MAX : (uint32_t(1)<<widths[i])-1; }
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGBlockDevWriteAckQueueWrapper" {
      firrtl.module @GGBlockDevWriteAckQueueWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %blockdev_req_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, len: uint<32>, offset: uint<32>, write: uint<1>>>,
        out %blockdev_data_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, data: uint<64>>>,
        in %blockdev_rresp_enq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<tag: uint<1>, data: uint<64>>>,
        in %blockdev_wack_enq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<1>>,
        in %blockdev_info: !firrtl.bundle<nsectors: uint<32>, max_req_len: uint<32>>,
        in %blockdev_timing: !firrtl.bundle<returnWrite: uint<1>, readRespBusy: uint<1>, wAckStallN flip: uint<1>, rRespStallN flip: uint<1>, tCycle flip: uint<24>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"blockdev_req_deq.bits.offset", "blockdev_data_deq.valid", "blockdev_rresp_enq.bits.data",
      "blockdev_wack_enq.bits", "blockdev_info.nsectors", "blockdev_timing.wAckStallN", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGBlockDevWriteAckQueueWrapper|GGBlockDevWriteAckQueueWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevMMIOBank(c, error)), error);
  require(succeeded(verify(*root)), "bank IR verification failed");
  Interpreter sim(named(c, "GGBlockDevMMIOBank")); std::array<uint32_t,26> expected;
  for (unsigned i=0;i<26;++i) expected[i]=(i<2 || pulse(i)) ? 1 : (0xA5 & mask(i));
  std::mt19937 random(200); unsigned resetWrites=0, overrides=0, retriggers=0, forbidden=0, unstroked=0;
  auto check=[&]() {
    for(unsigned i=0;i<26;++i) {
      std::string lane= "read["+std::to_string(i)+"]";
      require(sim.output(9,lane+".bits")==((i==2 || i==3)?0:expected[i]), "readback differs at slot "+std::to_string(i));
      require(sim.output(9,lane+".valid")==1 && sim.output(9,"write["+std::to_string(i)+"].ready")==1, "register handshake differs");
    }
    require(sim.output(2,"ready")==expected[9] && sim.output(3,"ready")==expected[14], "dequeue pulse differs");
    require(sim.output(4,"valid")==expected[18] && sim.output(4,"bits.tag")==expected[17] &&
        sim.output(4,"bits.data")==((uint64_t(expected[15])<<32)|expected[16]), "read response enqueue differs");
    require(sim.output(5,"valid")==expected[21] && sim.output(5,"bits")==expected[20], "write ack enqueue differs");
    require(sim.output(7,"nsectors")==expected[2] && sim.output(7,"max_req_len")==expected[3], "geometry differs");
    require(sim.output(8,"read_latency")==expected[0] && sim.output(8,"write_latency")==expected[1], "latency differs");
  };
  // Each word is written independently across all 64 sampled status combinations,
  // both reset states and all strobes; then arbitrary simultaneous transactions.
  constexpr unsigned exhaustive=26*64*2*16, randomized=16384;
  for(unsigned cycle=0;cycle<exhaustive+randomized;++cycle) {
    bool reset; unsigned statuses, strobe; uint32_t writeMask;
    if(cycle<exhaustive) {
      unsigned x=cycle; strobe=x%16; x/=16; reset=x%2; x/=2; statuses=x%64; x/=64; writeMask=1u<<x;
    } else { reset=(random()&15)==0; statuses=random()&63; strobe=random()&15; writeMask=random()&((1u<<26)-1); }
    bool reqValid=statuses&1, dataValid=statuses&2, rReady=statuses&4, wReady=statuses&8, wStallN=statuses&16, rStallN=statuses&32;
    uint32_t offset=random(),len=random(); bool write=random()&1,reqTag=random()&1,dataTag=random()&1;
    uint64_t data=(uint64_t(random())<<32)|random(); std::array<uint32_t,26> words; for(auto &v:words) v=random();
    sim.memo.clear(); auto input=[&](unsigned p,llvm::StringRef field,uint64_t v) { sim.memo[sim.key(sim.arg(p))+"."+field.str()]=v; };
    sim.memo[sim.key(sim.arg(1))]=reset;
    input(2,"valid",reqValid); input(2,"bits.write",write); input(2,"bits.offset",offset); input(2,"bits.len",len); input(2,"bits.tag",reqTag);
    input(3,"valid",dataValid); input(3,"bits.data",data); input(3,"bits.tag",dataTag);
    input(4,"ready",rReady); input(5,"ready",wReady); input(6,"wAckStallN",wStallN); input(6,"rRespStallN",rStallN); input(9,"wstrb",strobe);
    unsigned reads2=random()&1, reads3=random()&1;
    for(unsigned i=0;i<26;++i) {
      input(9,"write["+std::to_string(i)+"].valid",bool(writeMask&(1u<<i)));
      input(9,"write["+std::to_string(i)+"].bits",words[i]);
      input(9,"read["+std::to_string(i)+"].ready",i==2?reads2:i==3?reads3:random()&1);
    }
    check(); unsigned assertions=0, failures=0;
    for(auto a:sim.module.getOps<AssertOp>()) { ++assertions; failures+=sim.eval(a.getEnable())&&!sim.eval(a.getPredicate()); }
    require(assertions==2 && failures==(reset?0:reads2+reads3), "write-only assertion policy differs"); forbidden+=failures;
    auto next=expected;
    next[4]=reqValid; next[5]=write; next[6]=offset; next[7]=len; next[8]=reqTag;
    next[10]=dataValid; next[11]=data>>32; next[12]=uint32_t(data); next[13]=dataTag;
    next[19]=rReady; next[22]=wReady; next[23]=reqValid||dataValid; next[24]=!wStallN; next[25]=!rStallN;
    for(unsigned i=0;i<26;++i) {
      if(pulse(i)) next[i]=0;
      if(writeMask&(1u<<i)) {
        overrides+=(i>=4 && !pulse(i) && !(i>=15 && i<=17) && i!=20);
        next[i]=words[i]&mask(i); resetWrites+=reset; unstroked+=strobe==0;
      }
      if(reset && (i<2 || pulse(i))) next[i]=i<2?256:0;
      retriggers+=pulse(i)&&expected[i]&&next[i];
    }
    sim.edge(); sim.memo.clear(); expected=next; check();
  }
  require(resetWrites && overrides && retriggers && forbidden && unstroked, "MMIO coverage missing");
  llvm::outs()<<"BlockDev MMIO: "<<exhaustive+randomized<<" cycles, "<<resetWrites<<" reset writes, "<<overrides
      <<" status overrides, "<<retriggers<<" retriggered pulses, "<<forbidden<<" forbidden reads, "<<unstroked<<" writes with zero strobes\n";
}
void mapping(MLIRContext &context) {
  auto root=fixture(context); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevMMIOBank(c,error)),error);
  auto top=named(c,"GGBlockDevMMIOWrapper"), bank=named(c,"GGBlockDevMMIOBank");
  require(top.getNumPorts()==6 && top.getPortName(4)=="blockdev_latency" && top.getPortName(5)=="blockdevBridge_mcr" &&
      top.getPortDirection(4)==Direction::Out && top.getPortDirection(5)==Direction::Out,"wrong MMIO wrapper boundary");
  auto regs=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters"); require(regs && regs.size()==26,"register map missing");
  for(unsigned i=0;i<26;++i) {
    auto reg=cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name")==regNames[i] && reg.getAs<IntegerAttr>("offset").getInt()==4*i &&
        reg.getAs<BoolAttr>("readable").getValue()==(i!=2 && i!=3) && reg.getAs<BoolAttr>("writeable").getValue(), "register metadata differs");
  }
  std::map<std::string,InstanceOp> instances; for(auto inst:top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(),inst);
  auto inner=instances.at("sim"), mmio=instances.at("blockdevRegisters");
  auto bulk=[&](Value dst,Value src) { for(auto conn:top.getOps<ConnectOp>()) if(conn.getDest()==dst && conn.getSrc()==src) return true; return false; };
  require(bulk(mmio.getResult(2),inner.getResult(2)) && bulk(mmio.getResult(3),inner.getResult(3)) &&
      bulk(inner.getResult(4),mmio.getResult(4)) && bulk(inner.getResult(5),mmio.getResult(5)) &&
      bulk(inner.getResult(6),mmio.getResult(7)) && bulk(top.getBodyBlock()->getArgument(4),mmio.getResult(8)) &&
      bulk(top.getBodyBlock()->getArgument(5),mmio.getResult(9)), "queue/geometry/latency/MCR wiring differs");
  for(unsigned i=0;i<2;++i) {
    bool connected=false; for(auto conn:top.getOps<StrictConnectOp>()) connected|=conn.getDest()==mmio.getResult(i) && conn.getSrc()==top.getBodyBlock()->getArgument(i);
    require(connected,"MMIO bank must use host clock/reset");
  }
  for(auto name:{"wAckStallN","rRespStallN"}) {
    bool connected=false;
    for(auto conn:top.getOps<StrictConnectOp>()) {
      auto dst=conn.getDest().getDefiningOp<SubfieldOp>(), src=conn.getSrc().getDefiningOp<SubfieldOp>();
      connected|=dst && src && dst.getInput()==mmio.getResult(6) && src.getInput()==inner.getResult(7) && dst.getFieldName()==name && src.getFieldName()==name;
    }
    require(connected,"timing status sample wiring differs");
  }
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos && annos.size()==8,"annotations disappeared");
  for(unsigned i=0;i<8;++i) {
    auto target=cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue();
    require(target.starts_with(i<5?"~GGBlockDevMMIOWrapper|GGBlockDevWriteAckQueueWrapper>":"~GGBlockDevMMIOWrapper|GGBlockDevMMIOWrapper>"),"annotation identity differs");
  }
  require(succeeded(verify(*root)),"wrapper verification failed");
  std::string repeated; require(failed(goldengate::addBlockDevMMIOBank(c,repeated)),"repeated mapping accepted");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<17;++bad) {
    auto root=fixture(context); auto c=*root->getOps<CircuitOp>().begin(); auto top=named(c,"GGBlockDevWriteAckQueueWrapper"); OpBuilder b(&context);
    if(bad==0) c.setName("WrongTop");
    if(bad==1) c->removeAttr("rawAnnotations");
    if(bad>=2 && bad<=9) {
      SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[bad-2]=b.getStringAttr("MissingPort"); top.setPortNames(names);
    }
    if(bad==10 || bad==11) {
      SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[8]=b.getStringAttr(bad==10?"blockdevBridge_mcr":"blockdev_latency"); top.setPortNames(names);
    }
    if(bad==12) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),top,"used"); }
    if(bad==13 || bad==14) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==13?"GGBlockDevMMIOBank":"GGBlockDevMMIOWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
    if(bad==15 || bad==16) {
      unsigned p=bad==15?3:7; auto ports=top.getPorts(); ports[p].type=UIntType::get(&context,8,false);
      top.setPortTypes(b.getArrayAttr(llvm::to_vector(llvm::map_range(ports,[](PortInfo p)->Attribute { return TypeAttr::get(p.type); }))));
      top.getBodyBlock()->getArgument(p).setType(ports[p].type);
    }
    std::string before,after,error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addBlockDevMMIOBank(c,error)),"invalid MMIO boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before==after,"rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>(); behavior(context); mapping(context); rejection(context);
    llvm::outs()<<"BlockDev MMIO mapping, metadata, write-only assertions and 17 atomic rejections passed\n"; return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
}
