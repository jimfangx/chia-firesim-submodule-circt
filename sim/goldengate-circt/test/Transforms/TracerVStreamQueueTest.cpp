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
#include <vector>
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
  std::vector<APInt> memory = std::vector<APInt>(6144, APInt(512, 0));
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one RAM");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 6144 && ram.getDataType() == UIntType::get(m.getContext(), 512, false) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/ports");
    require(std::distance(m.getOps<RegResetOp>().begin(), m.getOps<RegResetOp>().end()) == 3 &&
        std::distance(m.getOps<RegOp>().begin(), m.getOps<RegOp>().end()) == 1, "wrong reset policy");
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt output(Value v) { return eval(drivers.at(key(v))); }
  APInt output(Value v, llvm::StringRef path) { return eval(drivers.at(key(v) + "." + path.str())); }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = width(v);
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp, RegOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w);
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)).isZero() ? 2 : 1));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bits.getInput()).lshr(bits.getLo()).zextOrTrunc(w);
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ugt(eval(op->getOperand(1))));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput() == ram.getResult(0) && f.getFieldName() == "data", "unsupported RAM access");
      require(output(ram.getResult(0), "en").getBoolValue(), "read disabled");
      n = memory.at(output(ram.getResult(0), "addr").getZExtValue());
    } else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    if (output(ram.getResult(1), "en").getBoolValue() && output(ram.getResult(1), "mask").getBoolValue())
      memory.at(output(ram.getResult(1), "addr").getZExtValue()) = output(ram.getResult(1), "data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGTracerVBridgeControlWrapper" {
      firrtl.module @GGTracerVBridgeControlWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %tracerv_stream: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<512>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context); SmallVector<Attribute> annos;
  for (auto name : {"tracerv_stream.bits", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGTracerVBridgeControlWrapper|GGTracerVBridgeControlWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVStreamQueue(c, error)), error);
  require(succeeded(verify(*root)), "queue IR invalid");
  Interpreter sim(named(c, "GGTracerVStreamQueue6144")); std::deque<APInt> expected;
  std::mt19937_64 random(160);
  unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0, collisions = 0;
  auto cycle = [&](bool reset, bool valid, bool ready) {
    APInt data(512, 0);
    for (unsigned j = 0; j < 8; ++j) data |= APInt(512, random()).shl(64 * j);
    sim.memo.clear(); sim.put(sim.arg(1), "", reset);
    sim.put(sim.arg(2), "valid", valid); sim.memo[sim.key(sim.arg(2)) + ".bits"] = data;
    sim.put(sim.arg(3), "ready", ready);
    bool enqReady = sim.output(sim.arg(2), "ready").getBoolValue();
    bool deqValid = sim.output(sim.arg(3), "valid").getBoolValue();
    require(enqReady == (expected.size() < 6144) && deqValid == !expected.empty(), "occupancy differs");
    require(sim.output(sim.arg(4)).getZExtValue() == expected.size(), "count differs");
    if (deqValid) require(sim.output(sim.arg(3), "bits") == expected.front(), "512-bit stream ordering differs");
    bool push = valid && enqReady, pop = ready && deqValid;
    fullBlocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    simultaneous += push && pop; pushes += push; pops += pop; resetWrites += reset && push;
    auto oldMemory = sim.memory;
    unsigned address = sim.output(sim.ram.getResult(1), "addr").getZExtValue();
    unsigned oldDeq = 0;
    for (auto r : sim.module.getOps<RegResetOp>()) if (r.getName() == "deq_ptr_value") oldDeq = sim.state.lookup(r.getResult()).getZExtValue();
    unsigned readAddress = pop ? (oldDeq + 1) % 6144 : oldDeq;
    collisions += push && address == readAddress;
    if (pop) expected.pop_front(); if (push) expected.push_back(data);
    if (reset) expected.clear(); if (push) oldMemory.at(address) = data;
    sim.edge(); require(sim.memory == oldMemory, "reset gates write or clears RAM");
    sim.memo.clear();
    require(sim.output(sim.ram.getResult(0), "addr").getZExtValue() == readAddress,
        "synchronous lookahead address/reset policy differs");
    require(sim.output(sim.arg(3), "bits") == sim.memory.at(readAddress), "synchronous RAM collision differs");
  };
  cycle(true, false, false);
  for (unsigned round = 0; round < 3; ++round) {
    for (unsigned i = 0; i < 6150; ++i) cycle(false, true, false);
    cycle(false, true, true); // full cannot push on a simultaneous dequeue
    for (unsigned i = 0; i < 6150; ++i) cycle(false, false, true);
    cycle(false, true, true); // empty cannot bypass
  }
  cycle(true, true, true); // RAM write and read address still active on reset
  for (unsigned i = 0; i < 20000; ++i) cycle(i % 7999 == 0, random() & 1, random() & 1);
  require(pushes > 18000 && pops > 18000 && fullBlocked && emptyBlocked && resetWrites && simultaneous && collisions,
      "coverage missing");
  llvm::outs() << "6144x512 stream queue: " << pushes << " pushes, " << pops << " pops, " << collisions
               << " read/write collisions checked\n";
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVStreamQueue(c, error)), error);
  auto top = named(c, "GGTracerVStreamQueueWrapper");
  require(top.getNumPorts() == 5 && top.getPortName(4) == "tracerv_stream_count", "count boundary missing");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGTracerVStreamQueueWrapper|GGTracerVBridgeControlWrapper>tracerv_stream.bits",
      "~GGTracerVStreamQueueWrapper|GGTracerVStreamQueueWrapper>other",
      "~GGTracerVStreamQueueWrapper|GGTracerVStreamQueueWrapper>hostReset"};
  require(annos.size() == 3, "annotations lost");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == targets[i], "producer/copied target differs");
  InstanceOp inner, fifo;
  for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getModuleName() == "GGTracerVStreamQueue6144") fifo = inst; else inner = inst;
  }
  require(inner && fifo, "wrapper needs producer and queue");
  auto bulk = [&](Value dest, Value src) {
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  auto wire = [&](Value dest, Value src) {
    for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(bulk(fifo.getResult(2), inner.getResult(2)) && bulk(arg(2), fifo.getResult(3)), "producer/consumer direction differs");
  require(wire(fifo.getResult(0), arg(0)) && wire(fifo.getResult(1), arg(1)) && wire(arg(4), fifo.getResult(4)), "clock/reset/count differs");
  auto q = named(c, "GGTracerVStreamQueue6144");
  require(q->getAttrOfType<DictionaryAttr>("goldengate.streamParameters").getAs<IntegerAttr>("depth").getInt() == 6144 &&
      (*q.getOps<MemOp>().begin())->getAttrOfType<StringAttr>("goldengate.ramStyle") == "ULTRA", "stream/RAM metadata missing");
}
void expandedMapping(MLIRContext &context) {
  constexpr llvm::StringLiteral input = "GGTracerVTriggerWrapper";
  constexpr llvm::StringLiteral output = "GGPrintTracerVStreamQueueWrapper";
  auto printed = [](Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; };
  for (unsigned bad = 0; bad < 8; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    auto inner = named(c, "GGTracerVBridgeControlWrapper"); OpBuilder b(&context);
    inner.setName(input); c.setName(input);
    auto token = inner.getPortType(2);
    inner.insertPorts({{4, PortInfo(b.getStringAttr("print_stream_0"), token, Direction::Out)}});
    auto registry = b.getArrayAttr({b.getDictionaryAttr({
        b.getNamedAttr("module", b.getStringAttr("PrintHost")),
        b.getNamedAttr("stream", b.getStringAttr("print_stream_0"))})});
    inner->setAttr("goldengate.printHostBindings", registry);
    auto targets = b.getArrayAttr({
        b.getStringAttr("~GGTracerVTriggerWrapper|GGTracerVTriggerWrapper>tracerv_stream.bits"),
        b.getStringAttr("~GGTracerVTriggerWrapper|GGTracerVTriggerWrapper>print_stream_0.bits"),
        b.getStringAttr("~GGTracerVTriggerWrapper|GGTracerVTriggerWrapper>hostClock")});
    auto nested = b.getDictionaryAttr({b.getNamedAttr("targets", targets)});
    auto annotation = b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("nested", nested)});
    c->setAttr("rawAnnotations", b.getArrayAttr({annotation}));
    StringRef selected = input, wrapper = output;
    if (bad == 1) selected = "WrongTop";
    if (bad == 2) wrapper = input;
    if (bad == 3) wrapper = "";
    if (bad == 4) wrapper = "GGTracerVStreamQueue6144";
    if (bad == 5) c->removeAttr("rawAnnotations");
    if (bad == 6) {
      auto names = SmallVector<Attribute>(inner.getPortNames().begin(), inner.getPortNames().end());
      names[4] = b.getStringAttr("tracerv_stream_count"); inner.setPortNames(names);
    }
    if (bad == 7) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr(output), inner.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before = printed(*root), innerBefore = printed(inner), error;
    auto result = goldengate::attachTracerVStreamQueue(c, selected, wrapper, error);
    if (bad) require(failed(result) && !error.empty() && printed(*root) == before,
                     "expanded queue rejection mutated IR");
    else {
      require(succeeded(result) && succeeded(verify(*root)), error);
      require(printed(inner) == innerBefore && inner->getAttr("goldengate.printHostBindings") == registry,
              "expanded queue changed Print host body/registry");
      auto top = named(c, output); InstanceOp sim, fifo;
      for (auto i : top.getOps<InstanceOp>()) if (i.getModuleName() == input) sim = i; else fifo = i;
      require(sim && fifo && top.getNumPorts() == 6 && top.getPortName(5) == "tracerv_stream_count",
              "expanded queue/count boundary differs");
      bool printForwarded = false;
      for (auto conn : top.getOps<ConnectOp>())
        printForwarded |= conn.getDest() == top.getBodyBlock()->getArgument(4) && conn.getSrc() == sim.getResult(4);
      require(printForwarded, "expanded Print stream not forwarded");
      auto row = cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
      auto targets = row.getAs<DictionaryAttr>("nested").getAs<ArrayAttr>("targets");
      const StringRef expected[]{
          "~GGPrintTracerVStreamQueueWrapper|GGTracerVTriggerWrapper>tracerv_stream.bits",
          "~GGPrintTracerVStreamQueueWrapper|GGPrintTracerVStreamQueueWrapper>print_stream_0.bits",
          "~GGPrintTracerVStreamQueueWrapper|GGPrintTracerVStreamQueueWrapper>hostClock"};
      for (unsigned i = 0; i < 3; ++i) require(cast<StringAttr>(targets[i]) == expected[i], "expanded recursive target differs");
    }
  }
  llvm::outs() << "Expanded Print stream/registry forwarding and seven atomic queue rejections passed\n";
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 6; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    auto top = named(c, "GGTracerVBridgeControlWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 5) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[bad == 2 ? 2 : 3] = b.getStringAttr(bad == 2 ? "WrongStream" : "tracerv_stream_count"); top.setPortNames(names);
    }
    if (bad == 3) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 4) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGTracerVStreamQueue6144"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addTracerVStreamQueue(c, error)), "invalid stream boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); expandedMapping(context); rejection(context);
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
