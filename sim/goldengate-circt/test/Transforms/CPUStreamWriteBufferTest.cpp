// See LICENSE for license details.
#include "goldengate/TracerVTokenEngine.h"
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
// Interpret the generated operations, including actual MemOp read/write ports.
// A separate deque below checks ordering/occupancy without using pointer logic.
struct Interpreter {
  FModuleOp module;
  MemOp ram;
  std::map<std::string, std::array<uint64_t, 2>> memory;
  std::map<std::string, unsigned> widths;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 2 && isa<BundleType>(ram.getDataType()) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    unsigned regs = 0;
    for (auto reg : m.getOps<RegResetOp>()) { require(reg.getResult().getType() == UIntType::get(m.getContext(), 1, false), "wrong pointer width"); ++regs; }
    require(regs == 3, "queue needs three control bits");
    for (auto f : cast<BundleType>(ram.getDataType()).getElements()) {
      widths[f.name.str()] = *cast<UIntType>(f.type).getWidth();
      memory[f.name.str()] = {0, 1};
    }
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      auto parent = f.getInput().getDefiningOp<SubfieldOp>();
      require(parent && parent.getInput() == ram.getResult(0) && parent.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en") == 1, "queue read disabled");
      n = memory.at(f.getFieldName().str()).at(drive(ram.getResult(0), "addr"));
    } else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = *cast<UIntType>(v.getType()).getWidth();
    if (width < 64) n &= (1ULL << width) - 1;
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    Value writer = ram.getResult(1);
    if (drive(writer, "en")) for (auto item : widths) {
      require(drive(writer, "mask." + item.first) == 1, "payload mask disabled");
      memory.at(item.first).at(drive(writer, "addr")) = drive(writer, "data." + item.first);
    }
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGCPUStreamWriteWrapper" {
      firrtl.module @GGCPUStreamWriteWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %cpu_stream_aw_ready: !firrtl.uint<1>, in %cpu_stream_aw_valid: !firrtl.uint<1>,
        in %cpu_stream_aw_bits_id: !firrtl.uint<16>, in %cpu_stream_aw_bits_size: !firrtl.uint<3>,
        out %cpu_stream_w_ready: !firrtl.uint<1>, in %cpu_stream_w_valid: !firrtl.uint<1>,
        in %cpu_stream_w_bits_strb: !firrtl.uint<64>, out %other: !firrtl.uint<8>,
        out %cpu_stream_b_bits_id: !firrtl.uint<16>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto n : {"cpu_stream_aw_valid", "cpu_stream_w_bits_strb", "other", "hostReset", "cpu_stream_b_bits_id"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGCPUStreamWriteWrapper|GGCPUStreamWriteWrapper>" + std::string(n)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addCPUStreamWriteBuffer(c, error)), error);
  require(succeeded(verify(*root)), "queue IR verification failed");
  std::mt19937_64 random(164);
  for (auto name : {"GGCPUStreamAWQueue2", "GGCPUStreamWQueue2"}) {
    Interpreter sim(named(c, name)); std::deque<std::map<std::string, uint64_t>> expected;
    unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0;
    auto cycle = [&](bool reset, bool valid, bool ready) {
      sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
      sim.memo[sim.key(sim.arg(2)) + ".valid"] = valid; sim.memo[sim.key(sim.arg(3)) + ".ready"] = ready;
      std::map<std::string, uint64_t> data;
      for (auto f : sim.widths) {
        uint64_t n = random(); if (f.second < 64) n &= (1ULL<<f.second)-1;
        sim.memo[sim.key(sim.arg(2)) + ".bits." + f.first] = data[f.first] = n;
      }
      bool enqReady = sim.drive(sim.arg(2), "ready"), deqValid = sim.drive(sim.arg(3), "valid");
      require(enqReady == (expected.size() < 2) && deqValid == !expected.empty(), "queue occupancy differs");
      bool push = valid && enqReady, pop = ready && deqValid;
      if (deqValid) for (auto item : sim.widths)
        require(sim.drive(sim.arg(3), "bits." + item.first) == expected.front().at(item.first), "payload reordered or overwritten");
      auto memBefore = sim.memory;
      unsigned address = sim.drive(sim.ram.getResult(1), "addr");
      if (pop) expected.pop_front(); if (push) expected.push_back(data); if (reset) expected.clear();
      sim.edge();
      for (auto item : sim.widths) for (unsigned i = 0; i < 2; ++i)
        require(sim.memory[item.first][i] == (push && i == address ? data[item.first] : memBefore[item.first][i]), "RAM write/reset semantics differ");
      pushes += push; pops += pop; simultaneous += push && pop; resetWrites += reset && push;
      fullBlocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    };
    // Filling while stalled is the actual empty-sink configuration. The first
    // payload remains visible at the engine until reset, regardless of later AW/W.
    for (unsigned i = 0; i < 100; ++i) { cycle(true, true, false); cycle(false, true, false); cycle(false, true, false); cycle(false, true, false); }
    for (unsigned i = 0; i < 20000; ++i) cycle((random()%23)==0, random()&1, random()&1);
    require(pushes > 1000 && pops > 1000 && fullBlocked > 100 && emptyBlocked > 100 && resetWrites > 100 && simultaneous > 100, "insufficient queue corner coverage");
    llvm::outs() << name << ": 20400 cycles, pushes " << pushes << ", pops " << pops << ", reset writes " << resetWrites << ", full/empty blocked " << fullBlocked << "/" << emptyBlocked << "\n";
  }
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addCPUStreamWriteBuffer(c, error)), error);
  auto top = named(c, "GGCPUStreamWriteBufferWrapper");
  require(top.getNumPorts() == 13 && top.getPortName(11) == "cpu_stream_aw_bits_addr" && top.getPortType(11) == UIntType::get(&context,64,false) && top.getPortName(12) == "cpu_stream_aw_bits_len" && top.getPortType(12) == UIntType::get(&context,8,false), "AW geometry differs");
  auto awMod = named(c,"GGCPUStreamAWQueue2"), wMod = named(c,"GGCPUStreamWQueue2");
  for (auto q : {awMod, wMod}) require(q->getAttrOfType<IntegerAttr>("goldengate.queueDepth").getInt() == 2 && !q->getAttrOfType<BoolAttr>("goldengate.queueFlow").getValue() && !q->getAttrOfType<BoolAttr>("goldengate.queuePipe").getValue(), "queue configuration differs");
  std::map<std::string, InstanceOp> instances;
  for (auto i : top.getOps<InstanceOp>()) instances.emplace(i.getName().str(),i);
  require(instances.size()==3, "queue instances missing");
  auto sim=instances.at("sim"), aw=instances.at("awQueue"), w=instances.at("wQueue");
  Interpreter keys(awMod);
  std::map<std::string,std::string> wires;
  for (auto conn : top.getOps<StrictConnectOp>()) require(wires.emplace(keys.key(conn.getDest()), keys.key(conn.getSrc())).second,"duplicate wrapper driver");
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  auto wire=[&](std::string d,std::string s) { require(wires.at(d)==s,"buffer wrapper wiring differs"); };
  for (auto q : {aw,w}) { wire(keys.key(q.getResult(0)),keys.key(arg(0))); wire(keys.key(q.getResult(1)),keys.key(arg(1))); }
  for (auto pair : {std::make_pair(aw,2u),std::make_pair(w,6u)}) {
    auto q=pair.first; unsigned ready=pair.second;
    wire(keys.key(arg(ready)),keys.key(q.getResult(2))+".ready");
    wire(keys.key(q.getResult(2))+".valid",keys.key(arg(ready+1)));
    wire(keys.key(q.getResult(3))+".ready",keys.key(sim.getResult(ready)));
    wire(keys.key(sim.getResult(ready+1)),keys.key(q.getResult(3))+".valid");
  }
  for (auto item : {std::make_pair(4u,"id"),std::make_pair(5u,"size"),std::make_pair(8u,"strb")}) {
    auto q=item.first==8 ? w : aw;
    wire(keys.key(q.getResult(2))+".bits."+item.second,keys.key(arg(item.first)));
    wire(keys.key(sim.getResult(item.first)),keys.key(q.getResult(3))+".bits."+item.second);
  }
  wire(keys.key(aw.getResult(2))+".bits.addr",keys.key(arg(11))); wire(keys.key(aw.getResult(2))+".bits.len",keys.key(arg(12)));
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size()==5,"annotations lost");
  for (auto [i,a] : llvm::enumerate(annos)) require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue().starts_with(i<2 ? "~GGCPUStreamWriteBufferWrapper|GGCPUStreamWriteWrapper>" : "~GGCPUStreamWriteBufferWrapper|GGCPUStreamWriteBufferWrapper>"),"target identity differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad=0;bad<8;++bad) {
    auto root=fixture(context);auto c=*root->getOps<CircuitOp>().begin();auto top=named(c,"GGCPUStreamWriteWrapper");OpBuilder b(&context);
    if (bad==0)c.setName("WrongTop"); if (bad==1)c->removeAttr("rawAnnotations");
    if (bad==2 || bad==3) { SmallVector<Attribute> names(top.getPortNames().begin(),top.getPortNames().end()); names[bad==2 ? 1 : 9]=b.getStringAttr(bad==2 ? "WrongReset" : "cpu_stream_aw_bits_addr");top.setPortNames(names); }
    if(bad==4) {b.setInsertionPointToStart(top.getBodyBlock());b.create<InstanceOp>(c.getLoc(),top,"used");}
    if(bad>=5) {b.setInsertionPointToEnd(c.getBodyBlock());b.create<FModuleOp>(c.getLoc(),b.getStringAttr(bad==5 ? "GGCPUStreamAWQueue2" : bad==6 ? "GGCPUStreamWQueue2" : "GGCPUStreamWriteBufferWrapper"),top.getConventionAttr(),ArrayRef<PortInfo>{});}
    std::string before,after,error; {llvm::raw_string_ostream out(before);root->print(out);}
    require(failed(goldengate::addCPUStreamWriteBuffer(c,error)),"unsupported boundary accepted");
    {llvm::raw_string_ostream out(after);root->print(out);}require(before==after,"rejection mutated IR");
  }
}
}
int main() {
  try {MLIRContext context;context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();behavior(context);mapping(context);rejection(context);return 0;}
  catch(const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
