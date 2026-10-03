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
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = cast<UIntType>(r.getResult().getType()).getWidthOrSentinel() == 2 ? 3 : 0xA5A5A5A5; }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 2 && reset == 0, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGLoadMemRequestWrapper" {
      firrtl.module @GGLoadMemRequestWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %loadmem_write_ready: !firrtl.uint<1>, in %loadmem_write_valid: !firrtl.uint<1>,
        in %loadmem_write_bits_addr: !firrtl.uint<34>, in %loadmem_write_bits_len: !firrtl.uint<34>,
        out %loadmem_zero_ready: !firrtl.uint<1>, in %loadmem_zero_valid: !firrtl.uint<1>,
        out %loadmem_zero_finished: !firrtl.uint<1>, out %other: !firrtl.uint<8>,
        in %dataValid: !firrtl.uint<1>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"loadmem_write_bits_addr", "loadmem_write_bits_len", "other", "hostReset", "dataValid"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGLoadMemRequestWrapper|GGLoadMemRequestWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemWriteMMIO(c, error)), error);
  require(succeeded(verify(*root)), "bank IR verification failed");
  Interpreter sim(named(c, "GGLoadMemWriteMMIOBank"));
  uint32_t high = 3, low = 0xA5A5A5A5; std::mt19937 random(170);
  unsigned resetWrites=0, unstrokedWrites=0, stalledRequests=0, illegalAccesses=0;
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    // Independently enumerate reset, address writes, request valids, ready/status
    // and forbidden accesses. Random UInt32 payloads exercise high truncation.
    bool reset=cycle&1, ready=cycle&2, zeroReady=cycle&4, finished=cycle&8;
    unsigned mask=(cycle>>4)&31, strobe=(cycle>>9)&15;
    sim.memo.clear();
    auto input=[&](unsigned i,uint64_t n) { sim.memo[sim.key(sim.arg(i))]=n; };
    auto mcr=[&](const std::string &name,uint64_t n) { sim.memo[sim.key(sim.arg(9))+"."+name]=n; };
    input(1,reset); input(2,ready); input(6,zeroReady); input(8,finished); mcr("wstrb",strobe);
    std::array<uint32_t,5> data; const unsigned slots[]{0,1,2,3,5};
    for (unsigned i=0;i<5;++i) {
      data[i]=random(); mcr("write_"+std::to_string(slots[i])+".valid",bool(mask&(1<<i)));
      mcr("write_"+std::to_string(slots[i])+".bits",data[i]);
      mcr("read_"+std::to_string(slots[i])+".ready",bool(cycle&(1<<(i+10))));
      uint64_t expected = i==0 ? high : i==1 ? low : i==4 ? finished : 0;
      require(sim.output(9,"read_"+std::to_string(slots[i])+".bits")==expected,"MMIO read data differs");
      require(sim.output(9,"read_"+std::to_string(slots[i])+".valid")==unsigned(i!=2 && i!=3),"MMIO read permission differs");
      require(sim.output(9,"write_"+std::to_string(slots[i])+".ready")==unsigned(i==2 ? ready : i==3 ? zeroReady : true),"MMIO write ready differs");
    }
    require(sim.eval(sim.arg(3))==bool(mask&4) && sim.eval(sim.arg(7))==bool(mask&8),"decoded request valid differs");
    require(sim.eval(sim.arg(4))==((uint64_t(high)<<32)|low),"request must use pre-edge address");
    require(sim.eval(sim.arg(5))==data[2],"request length must zero extend UInt32");
    unsigned assertions=0, failedAssertions=0;
    for (auto a : sim.module.getOps<AssertOp>()) {
      ++assertions; failedAssertions += sim.eval(a->getOperand(2)) && !sim.eval(a->getOperand(1));
    }
    unsigned forbidden = bool(cycle&(1<<12)) + bool(cycle&(1<<13)) + bool(mask&16);
    require(assertions==3 && failedAssertions==(reset ? 0 : forbidden),"access assertion reset/permission differs");
    illegalAccesses+=failedAssertions; resetWrites+=reset && (mask&3); unstrokedWrites+=!strobe && (mask&3);
    stalledRequests+=((mask&4)&&!ready) || ((mask&8)&&!zeroReady);
    if (mask&1) high=data[0]&3;
    if (mask&2) low=data[1];
    sim.edge(); sim.memo.clear();
    require(sim.output(9,"read_0.bits")==high && sim.output(9,"read_1.bits")==low,"address write/reset/strobe precedence differs");
  }
  require(resetWrites && unstrokedWrites && stalledRequests && illegalAccesses,"missing MMIO coverage");
  llvm::outs()<<"LoadMem write MMIO: 32768 cycles, "<<resetWrites<<" reset writes, "<<unstrokedWrites
      <<" zero-strobe writes, "<<stalledRequests<<" stalled requests, "<<illegalAccesses<<" forbidden accesses\n";
}
void mapping(MLIRContext &context) {
  auto root=fixture(context); auto c=*root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addLoadMemWriteMMIO(c,error)),error);
  auto top=named(c,"GGLoadMemWriteMMIOWrapper"), bank=named(c,"GGLoadMemWriteMMIOBank");
  require(top.getNumPorts()==5 && top.getPortName(4)=="loadmemWrite_mcr","wrong sparse bank wrapper ports");
  require(bank.getNumPorts()==10 && cast<BundleType>(bank.getPortType(9)).getNumElements()==11,"wrong decoded MCR geometry");
  auto regs=bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const llvm::StringRef names[]{"W_ADDRESS_H","W_ADDRESS_L","W_LENGTH","ZERO_OUT_DRAM","ZERO_FINISHED"};
  const unsigned slots[]{0,1,2,3,5}; require(regs && regs.size()==5,"missing register map");
  for(unsigned i=0;i<5;++i) {
    auto r=cast<DictionaryAttr>(regs[i]);
    require(r.getAs<StringAttr>("name")==names[i] && r.getAs<IntegerAttr>("offset").getInt()==4*slots[i] &&
        r.getAs<BoolAttr>("readable").getValue()==(i<2 || i==4) &&
        r.getAs<BoolAttr>("writeable").getValue()==(i<4),"wrong address map or permission");
  }
  std::map<std::string,InstanceOp> instances;
  for(auto inst:top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(),inst);
  auto inner=instances.at("sim"), mmio=instances.at("loadmemRegisters");
  auto strict=[&](Value d,Value s) { for(auto v:top.getOps<StrictConnectOp>()) if(v.getDest()==d && v.getSrc()==s) return true; return false; };
  for(unsigned i=0;i<2;++i) require(strict(mmio.getResult(i),top.getBodyBlock()->getArgument(i)),"wrong host clock/reset wiring");
  for(unsigned i=2;i<9;++i) {
    bool toBank=i==2 || i==6 || i==8;
    require(strict(toBank ? mmio.getResult(i) : inner.getResult(i),toBank ? inner.getResult(i) : mmio.getResult(i)),"wrong request direction/wiring");
  }
  bool bulk=false;
  for(auto v:top.getOps<ConnectOp>()) bulk |= v.getDest()==top.getBodyBlock()->getArgument(4) && v.getSrc()==mmio.getResult(9);
  require(bulk,"missing decoded MCR aggregate connection");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef modules[]{"GGLoadMemRequestWrapper","GGLoadMemRequestWrapper","GGLoadMemWriteMMIOWrapper","GGLoadMemWriteMMIOWrapper","GGLoadMemWriteMMIOWrapper"};
  const llvm::StringRef ports[]{"loadmem_write_bits_addr","loadmem_write_bits_len","other","hostReset","dataValid"};
  for(unsigned i=0;i<5;++i) require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue()==
      "~GGLoadMemWriteMMIOWrapper|"+modules[i].str()+">"+ports[i].str(),"annotation target identity differs");
}
void rejection(MLIRContext &context) {
  for(unsigned bad=0;bad<12;++bad) {
    auto root=fixture(context); auto c=*root->getOps<CircuitOp>().begin(); auto top=named(c,"GGLoadMemRequestWrapper"); OpBuilder b(&context);
    if(bad==0) c.setName("WrongTop");
    if(bad==1) c->removeAttr("rawAnnotations");
    if(bad>=2 && bad<=8) {
      SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[bad]=b.getStringAttr("wrong"); top.setPortNames(names);
    }
    if(bad==9) { SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[9]=b.getStringAttr("loadmem_write_extra"); top.setPortNames(names); }
    if(bad==10) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(),top,"used"); }
    if(bad==11) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(),b.getStringAttr("GGLoadMemWriteMMIOBank"),top.getConventionAttr(),ArrayRef<PortInfo>{}); }
    std::string before,after,error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addLoadMemWriteMMIO(c,error)),"invalid MMIO boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before==after,"rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    behavior(context); mapping(context); rejection(context);
    llvm::outs()<<"LoadMem write MMIO mapping, metadata, permissions and 12 atomic rejections passed\n"; return 0;
  } catch(const std::exception &e) { llvm::errs()<<e.what()<<'\n'; return 1; }
}
