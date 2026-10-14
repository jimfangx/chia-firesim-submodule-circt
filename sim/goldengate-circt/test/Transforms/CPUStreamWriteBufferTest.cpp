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
#include "mlir/IR/IRMapping.h"
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
void behavior(CircuitOp c) {
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
// Compose the exact write helper projected from a production boundary with
// the same AW/W/B attachment APIs as the compiler. Emitted standalone IR is
// used for differential RTL execution against the archived SFC queues.
void compose(MLIRContext &context, const char *input, const char *output,
             const char *readInput = nullptr) {
  auto root = parseSourceFile<ModuleOp>(input, &context);
  require(bool(root) && succeeded(verify(*root)), "invalid write helper input");
  auto c = *root->getOps<CircuitOp>().begin(); auto engine = named(c, "GGCPUStreamWrite");
  require(engine.getNumPorts() == 17 && engine.getPortName(2) == "stream", "needs single NIC write helper");
  OpBuilder b(&context); SmallVector<PortInfo> ports(engine.getPorts());
  ports[0].name = b.getStringAttr("hostClock"); ports[1].name = b.getStringAttr("hostReset");
  for (unsigned i = 3; i < ports.size(); ++i) ports[i].name = b.getStringAttr("cpu_stream_" + ports[i].name.str());
  FModuleOp readEngine;
  unsigned readStart = ports.size();
  if (readInput) {
    auto readRoot = parseSourceFile<ModuleOp>(readInput, &context);
    require(bool(readRoot) && succeeded(verify(*readRoot)), "invalid read helper input");
    auto readCircuit = *readRoot->getOps<CircuitOp>().begin();
    auto helper = named(readCircuit, "GGCPUStreamRead");
    require(helper.getNumPorts() == 16 && helper.getPortName(2) == "stream_0" &&
      helper.getPortName(3) == "stream_1", "needs NIC and TracerV read helpers");
    readEngine = cast<FModuleOp>(helper->clone()); c.getBodyBlock()->push_back(readEngine);
    for (unsigned i = 2; i < helper.getNumPorts(); ++i) {
      auto p = helper.getPorts()[i];
      if (i >= 4) p.name = b.getStringAttr("cpu_stream_" + p.name.str());
      ports.push_back(p);
    }
  }
  b.setInsertionPointToEnd(c.getBodyBlock());
  auto top = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGCPUStreamWriteWrapper"), engine.getConventionAttr(), ports);
  b.setInsertionPointToStart(top.getBodyBlock()); auto sim = b.create<InstanceOp>(c.getLoc(), engine, "write");
  for (auto [i, p] : llvm::enumerate(ports)) {
    if (i >= readStart) break;
    Value outer = top.getArgument(i), inner = sim.getResult(i);
    b.create<ConnectOp>(c.getLoc(), p.direction == Direction::In ? inner : outer,
                       p.direction == Direction::In ? outer : inner);
  }
  if (readEngine) {
    auto read = b.create<InstanceOp>(c.getLoc(), readEngine, "read");
    for (unsigned i = 0; i < readEngine.getNumPorts(); ++i) {
      unsigned port = i < 2 ? i : readStart + i - 2;
      auto p = ports[port]; Value outer = top.getArgument(port), inner = read.getResult(i);
      b.create<ConnectOp>(c.getLoc(), p.direction == Direction::In ? inner : outer,
                         p.direction == Direction::In ? outer : inner);
    }
  }
  c.setName("GGCPUStreamWriteWrapper"); SmallVector<Attribute> annotations;
  for (auto p : ports) annotations.push_back(b.getDictionaryAttr({
    b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
    b.getNamedAttr("target", b.getStringAttr("~GGCPUStreamWriteWrapper|GGCPUStreamWriteWrapper>" + p.name.str()))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations)); std::string error;
  for (unsigned bad=0;bad<9;++bad) {
    OwningOpRef<ModuleOp> copy(cast<ModuleOp>(root->clone()));
    auto cc=*copy->getOps<CircuitOp>().begin(); auto boundary=named(cc,"GGCPUStreamWriteWrapper");
    unsigned port=bad%3==0 ? 6 : bad%3==1 ? 7 : 11;
    if (bad<3) {
      auto names=llvm::to_vector(boundary.getPortNames());names[port]=b.getStringAttr("missing_live_field");boundary.setPortNames(names);
    } else if (bad<6) {
      auto types=llvm::to_vector(boundary.getPortTypes());auto wrong=UIntType::get(&context,2,false);
      types[port]=TypeAttr::get(wrong);boundary->setAttr("portTypes",b.getArrayAttr(types));boundary.getArgument(port).setType(wrong);
    } else {
      SmallVector<bool> directions(boundary.getPortDirections().begin(),boundary.getPortDirections().end());
      directions[port]=!directions[port];boundary.setPortDirections(directions);
    }
    std::string before,after; {llvm::raw_string_ostream out(before);copy->print(out);}
    require(failed(goldengate::addCPUStreamWriteBuffer(cc,error)),"bad live payload accepted");
    {llvm::raw_string_ostream out(after);copy->print(out);}require(before==after,"live rejection mutated IR");
  }
  require(succeeded(goldengate::addCPUStreamWriteBuffer(c, error)), error);
  auto buffered = named(c, "GGCPUStreamWriteBufferWrapper");
  require(buffered.getNumPorts() == ports.size(), "live payload duplicated or dropped");
  auto w = named(c, "GGCPUStreamWQueue2"); auto mem = *w.getOps<MemOp>().begin();
  require(cast<BundleType>(mem.getDataType()).getElementType("data") == UIntType::get(&context,512,false), "live W data absent");
  // Validate both directions of every added live payload connection.
  InstanceOp aw, wd, wrapped;
  for (auto i : buffered.getOps<InstanceOp>()) {
    if (i.getName() == "awQueue") aw=i;
    if (i.getName() == "wQueue") wd=i;
    if (i.getName() == "sim") wrapped=i;
  }
  Interpreter keys(named(c, "GGCPUStreamAWQueue2")); std::map<std::string,std::string> wires;
  for (auto conn : buffered.getOps<StrictConnectOp>())
    require(wires.emplace(keys.key(conn.getDest()),keys.key(conn.getSrc())).second,"duplicate live driver");
  for (auto [port,name] : {std::pair<unsigned,const char *>{6,"addr"},{7,"len"},{11,"data"}}) {
    auto q=port==11 ? wd : aw;
    require(wires.at(keys.key(q.getResult(2))+".bits."+name)==keys.key(buffered.getArgument(port)) &&
      wires.at(keys.key(wrapped.getResult(port)))==keys.key(q.getResult(3))+".bits."+name,"live payload bypasses queue");
  }
  require(succeeded(goldengate::attachCPUStreamWriteResponseBuffer(c,
    "GGCPUStreamWriteBufferWrapper","GGCPUStreamWriteResponseBufferWrapper",error)),error);
  require(succeeded(verify(*root)), "buffered composition invalid");
  auto final = named(c,"GGCPUStreamWriteResponseBufferWrapper");
  require(final.getNumPorts()==ports.size(),"response attachment changed live port inventory");
  auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(annos.size()==ports.size(),"composition lost annotations");
  for (auto [i,a] : llvm::enumerate(annos)) {
    auto name=ports[i].name.getValue();
    auto owner = name.starts_with("cpu_stream_aw_") || name.starts_with("cpu_stream_w_") ? "GGCPUStreamWriteWrapper" :
      name.starts_with("cpu_stream_b_") ? "GGCPUStreamWriteBufferWrapper" : "GGCPUStreamWriteResponseBufferWrapper";
    require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue()==
      "~GGCPUStreamWriteResponseBufferWrapper|"+std::string(owner)+">"+name.str(),"live annotation identity differs");
  }
  auto before = std::string(); {llvm::raw_string_ostream out(before);root->print(out);}
  require(failed(goldengate::addCPUStreamWriteBuffer(c,error)),"repeated buffer accepted");
  auto after = std::string(); {llvm::raw_string_ostream out(after);root->print(out);}
  require(before==after,"repeat mutated IR");
  if (readInput) {
    // Malformed AR and R preflight must not leave half-attached wrappers.
    auto atomicReject = [&](bool response) {
      for (unsigned bad = 0; bad < 3; ++bad) {
        OwningOpRef<ModuleOp> copy(cast<ModuleOp>(root->clone()));
        auto cc = *copy->getOps<CircuitOp>().begin(); auto boundary = named(cc,cc.getName());
        unsigned port = readStart + (response ? 11 : 5); // R.data / AR.addr
        if (bad == 0) {
          auto names=llvm::to_vector(boundary.getPortNames()); names[port]=b.getStringAttr("missing_field"); boundary.setPortNames(names);
        } else if (bad == 1) {
          auto types=llvm::to_vector(boundary.getPortTypes()); auto wrong=UIntType::get(&context,2,false);
          types[port]=TypeAttr::get(wrong); boundary->setAttr("portTypes",b.getArrayAttr(types)); boundary.getArgument(port).setType(wrong);
        } else {
          SmallVector<bool> directions(boundary.getPortDirections().begin(),boundary.getPortDirections().end());
          directions[port]=!directions[port]; boundary.setPortDirections(directions);
        }
        std::string before,after; {llvm::raw_string_ostream out(before);copy->print(out);}
        auto result = response ? goldengate::attachCPUStreamResponseBuffer(cc,cc.getName(),"RejectedR",error) :
          goldengate::attachCPUStreamReadBuffer(cc,cc.getName(),"RejectedAR",error);
        require(failed(result),"malformed AR/R accepted");
        {llvm::raw_string_ostream out(after);copy->print(out);} require(before==after,"AR/R rejection mutated IR");
      }
    };
    atomicReject(false);
    require(succeeded(goldengate::attachCPUStreamReadBuffer(c,
      "GGCPUStreamWriteResponseBufferWrapper","GGNICCPUStreamReadBufferWrapper",error)),error);
    atomicReject(true);
    require(succeeded(goldengate::attachCPUStreamResponseBuffer(c,
      "GGNICCPUStreamReadBufferWrapper","GGNICCPUStreamResponseBufferWrapper",error)),error);
    require(succeeded(verify(*root)),"duplex buffered composition invalid");
    auto final = named(c,c.getName()); require(final.getNumPorts()==ports.size(),"duplex ports changed");
    auto annos=c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size()==ports.size(),"duplex annotations lost");
    for (auto [i,a] : llvm::enumerate(annos)) {
      auto name=ports[i].name.getValue();
      auto owner = name.starts_with("cpu_stream_aw_") || name.starts_with("cpu_stream_w_") ? "GGCPUStreamWriteWrapper" :
        name.starts_with("cpu_stream_b_") ? "GGCPUStreamWriteBufferWrapper" :
        name.starts_with("cpu_stream_ar_") ? "GGCPUStreamWriteResponseBufferWrapper" :
        name.starts_with("cpu_stream_r_") ? "GGNICCPUStreamReadBufferWrapper" : "GGNICCPUStreamResponseBufferWrapper";
      require(cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue()==
        "~GGNICCPUStreamResponseBufferWrapper|"+std::string(owner)+">"+name.str(),"duplex target identity differs");
    }
    llvm::outs()<<"PASS live AW/W/B/AR/R composition, all 31 target identities, six atomic AR/R rejects\n";
  }
  std::error_code ec; llvm::raw_fd_ostream out(output,ec); require(!ec,"cannot write composed IR"); root->print(out);out<<'\n';
  llvm::outs()<<"PASS live AW/W/B composition, 512-bit payload wires, all 17 target identities, nine atomic live rejects and repeated rejection\n";
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
int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect,circt::hw::HWDialect>();
    if (argc == 4 && std::string(argv[1]) == "--compose") {
      compose(context,argv[2],argv[3]);
    } else if (argc == 5 && std::string(argv[1]) == "--compose-duplex") {
      compose(context,argv[2],argv[4],argv[3]);
    } else if (argc == 2) {
      auto root = parseSourceFile<ModuleOp>(argv[1], &context);
      require(bool(root) && succeeded(verify(*root)), "invalid expanded queue boundary");
      behavior(*root->getOps<CircuitOp>().begin());
    } else {
      require(argc == 1, "usage: goldengate-cpu-stream-write-buffer-test [boundary.mlir]");
      auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
      require(succeeded(goldengate::addCPUStreamWriteBuffer(c, error)) && succeeded(verify(*root)), error);
      behavior(c); mapping(context); rejection(context);
    }
    return 0;
  }
  catch(const std::exception &e) {llvm::errs()<<e.what()<<'\n';return 1;}
}
