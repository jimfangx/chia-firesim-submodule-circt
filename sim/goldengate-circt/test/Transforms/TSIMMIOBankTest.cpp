// See LICENSE for license details.
#include "goldengate/TSIMMIOBank.h"
#include "goldengate/ControlAddressDecode.h"
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
    require(regular == 6 && reset == 3, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
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
    firrtl.circuit "GGTSIWordQueuesWrapper" {
      firrtl.module @GGTSIWordQueuesWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %tsi_in_enq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>,
        out %tsi_out_deq: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<32>>,
        out %tsi_control: !firrtl.bundle<step_size flip: uint<32>, start flip: uint<1>, done: uint<1>>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annos;
  for (auto name : {"tsi_in_enq.bits", "tsi_out_deq.valid", "tsi_control.done", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGTSIWordQueuesWrapper|GGTSIWordQueuesWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
void phases(MLIRContext &context) {
  auto early = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
    firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })", &context);
  require(bool(early), "early fixture parse failed");
  auto c = *early->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  c->setAttr("rawAnnotations", b.getArrayAttr({b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Target")),
      b.getNamedAttr("target", b.getStringAttr("~GGControlErrorWrapper|GGControlErrorWrapper>other"))})}));
  auto top = named(c, "GGControlErrorWrapper"); auto topBefore = dump(top);
  auto annotations = c->getAttr("rawAnnotations"); FModuleOp bank; std::string error;
  require(succeeded(goldengate::materializeTSIMMIOBank(c, bank, error)), error);
  require(c.getName() == "GGControlErrorWrapper" && dump(top) == topBefore &&
      c->getAttr("rawAnnotations") == annotations && succeeded(verify(*early)),
      "early materialization changed top identity, ports or annotations");
  unsigned uses = 0; c.walk([&](InstanceOp i) { uses += i.getModuleName() == bank.getName(); });
  require(uses == 0, "materialization prematurely attached the bank");
  goldengate::ControlMMIOWidget tsi{"sentinel", 99};
  require(succeeded(goldengate::deriveControlMMIOWidget(c, "TSIBridgeModule_0", bank.getName(),
      {bank.getName()}, tsi, error, Direction::Out)) && tsi.registerCount == 9, error);
  const goldengate::ControlMMIOWidget widgets[]{tsi, {"stream", 1}};
  SmallVector<goldengate::ControlMMIORegion> regions;
  require(succeeded(goldengate::allocateControlMMIORegions(25, widgets, regions, error)) &&
      regions.size() == 2 && regions[0].name == tsi.name && regions[0].size == 64 &&
      regions[0].start == 0 && regions[1].start == 64, "standalone TSI bank allocation differs");
  auto before = dump(*early); FModuleOp unchanged = top;
  require(failed(goldengate::materializeTSIMMIOBank(c, unchanged, error)) &&
      unchanged == top && dump(*early) == before, "duplicate materialization changed result or IR");
  require(failed(goldengate::attachTSIMMIOBank(c, bank, error)) && dump(*early) == before,
      "premature attachment changed IR");
  goldengate::ControlMMIOWidget sentinel{"sentinel", 99, 8};
  require(failed(goldengate::deriveControlMMIOWidget(c, tsi.name, bank.getName(),
      {bank.getName()}, sentinel, error)) && sentinel.registerCount == 99 &&
      sentinel.customSize == 8 && dump(*early) == before, "wrong MCR direction accepted or mutated output");

  // The split API must preserve all hardware equations and target transfers.
  auto split = fixture(context), combined = fixture(context);
  auto splitCircuit = *split->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::materializeTSIMMIOBank(splitCircuit, bank, error)) &&
      succeeded(goldengate::attachTSIMMIOBank(splitCircuit, bank, error)), error);
  require(succeeded(goldengate::addTSIMMIOBank(*combined->getOps<CircuitOp>().begin(), error)), error);
  require(dump(*split) == dump(*combined) && succeeded(verify(*split)),
      "split bank construction changed hardware or annotations");

  // Null/foreign/wrong banks, each port's name/type/direction, arity and uses.
  for (unsigned bad = 0; bad < 23; ++bad) {
    auto root = fixture(context), foreign = fixture(context);
    auto circuit = *root->getOps<CircuitOp>().begin(); FModuleOp candidate;
    require(succeeded(goldengate::materializeTSIMMIOBank(circuit, candidate, error)), error);
    if (bad == 0) candidate = {};
    if (bad == 1) require(succeeded(goldengate::materializeTSIMMIOBank(
        *foreign->getOps<CircuitOp>().begin(), candidate, error)), error);
    if (bad == 2) candidate = named(circuit, "GGTSIWordQueuesWrapper");
    if (bad >= 3 && bad <= 21) {
      SmallVector<PortInfo> malformed(candidate.getPorts());
      if (bad == 21) malformed.pop_back();
      else {
        unsigned port = (bad - 3) / 3, mutation = (bad - 3) % 3;
        if (mutation == 0) malformed[port].name = b.getStringAttr("wrong");
        if (mutation == 1) malformed[port].type = UIntType::get(&context, 2);
        if (mutation == 2) malformed[port].direction = malformed[port].direction == Direction::In ? Direction::Out : Direction::In;
      }
      candidate.erase(); b.setInsertionPointToEnd(circuit.getBodyBlock());
      candidate = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("GGTSIMMIOBank"),
          ConventionAttr::get(&context, Convention::Internal), malformed);
    }
    if (bad == 22) {
      b.setInsertionPointToEnd(circuit.getBodyBlock());
      auto user = b.create<FModuleOp>(circuit.getLoc(), b.getStringAttr("User"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(circuit.getLoc(), candidate, "used");
    }
    auto before = dump(*root);
    require(failed(goldengate::attachTSIMMIOBank(circuit, candidate, error)) &&
        !error.empty() && dump(*root) == before, "invalid standalone bank accepted or mutated IR");
  }
  llvm::outs() << "TSI MMIO phases: early nine-word allocation, unchanged top/annotations, split/combined equivalence and 26 atomic rejections passed\n";
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTSIMMIOBank(c, error)), error);
  require(succeeded(verify(*root)), "bank IR verification failed");
  Interpreter sim(named(c, "GGTSIMMIOBank"));
  std::array<uint32_t, 9> expected{0xA5, 1, 1, 0xA5, 1, 1, 0xA5, 1, 1};
  std::mt19937 random(192);
  unsigned writeDuringReset = 0, statusOverride = 0, pulseRetrigger = 0;
  auto inputs = [&](bool reset, bool ready, bool valid, uint32_t bits, bool done,
                    unsigned writeMask, const std::array<uint32_t, 9> &data, unsigned strobes) {
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".ready"] = ready;
    sim.memo[sim.key(sim.arg(3)) + ".bits"] = bits;
    sim.memo[sim.key(sim.arg(3)) + ".valid"] = valid;
    sim.memo[sim.key(sim.arg(4)) + ".done"] = done;
    sim.memo[sim.key(sim.arg(5)) + ".wstrb"] = strobes;
    for (unsigned i = 0; i < 9; ++i) {
      std::string lane = sim.key(sim.arg(5)) + ".write[" + std::to_string(i) + "]";
      sim.memo[lane + ".valid"] = bool(writeMask & (1 << i));
      sim.memo[lane + ".bits"] = data[i];
      // Read backpressure does not affect register sampling or write acceptance.
      sim.memo[sim.key(sim.arg(5)) + ".read[" + std::to_string(i) + "].ready"] = bool(random() & 1);
    }
  };
  auto check = [&]() {
    for (unsigned i = 0; i < 9; ++i) {
      require(sim.output(5, "read[" + std::to_string(i) + "].bits") == expected[i], "read must see pre-edge register");
      require(sim.output(5, "read[" + std::to_string(i) + "].valid") == 1 &&
          sim.output(5, "write[" + std::to_string(i) + "].ready") == 1, "MCR register handshake differs");
    }
    require(sim.output(2, "bits") == expected[0] && sim.output(2, "valid") == expected[1] &&
        sim.output(3, "ready") == expected[5] && sim.output(4, "step_size") == expected[6] &&
        sim.output(4, "start") == expected[8], "queue/scheduler sees wrong MMIO state");
  };
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    // Exhaust all 512 write masks, reset, and three sampled status inputs.
    bool reset = cycle & 1, ready = cycle & 2, valid = cycle & 4, done = cycle & 8;
    unsigned writeMask = (cycle >> 4) & 511; uint32_t bits = random();
    std::array<uint32_t, 9> data;
    for (auto &value : data) value = random();
    inputs(reset, ready, valid, bits, done, writeMask, data, (cycle >> 9) & 15);
    check();
    std::array<uint32_t, 9> next{expected[0], 0, uint32_t(ready), bits, uint32_t(valid), 0,
        expected[6], uint32_t(done), 0};
    for (unsigned i = 0; i < 9; ++i) if (writeMask & (1 << i))
      next[i] = data[i] & ((i == 0 || i == 3 || i == 6) ? UINT32_MAX : 1);
    writeDuringReset += reset && writeMask;
    statusOverride += bool(writeMask & ((1 << 2) | (1 << 3) | (1 << 4) | (1 << 7)));
    if (reset) next[1] = next[5] = next[8] = 0;
    sim.edge(); sim.memo.clear(); expected = next; check();
  }
  // Arbitrary known state trajectories verify independently changing writes,
  // consecutive pulses and full-width words including the high bit.
  for (unsigned cycle = 0; cycle < 16384; ++cycle) {
    bool reset = (random() & 15) == 0, ready = random() & 1, valid = random() & 1, done = random() & 1;
    unsigned writeMask = random() & 511; uint32_t bits = random();
    std::array<uint32_t, 9> data; for (auto &v : data) v = random();
    inputs(reset, ready, valid, bits, done, writeMask, data, random() & 15); check();
    auto next = expected; next[1] = next[5] = next[8] = 0;
    next[2] = ready; next[3] = bits; next[4] = valid; next[7] = done;
    for (unsigned i = 0; i < 9; ++i) if (writeMask & (1 << i))
      next[i] = data[i] & ((i == 0 || i == 3 || i == 6) ? UINT32_MAX : 1);
    if (reset) next[1] = next[5] = next[8] = 0;
    pulseRetrigger += (next[1] && expected[1]) || (next[5] && expected[5]) || (next[8] && expected[8]);
    sim.edge(); sim.memo.clear(); expected = next; check();
  }
  require(writeDuringReset && statusOverride && pulseRetrigger, "bank coverage missing");
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTSIMMIOBank(c, error)), error);
  auto top = named(c, "GGTSIMMIOWrapper"), bank = named(c, "GGTSIMMIOBank");
  require(top.getNumPorts() == 4 && top.getPortName(3) == "tsiBridge_mcr" &&
      top.getPortDirection(3) == Direction::Out, "TSI queue/scheduler ports remained external");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  const llvm::StringRef names[]{"in_bits", "in_valid", "in_ready", "out_bits", "out_valid", "out_ready", "step_size", "done", "start"};
  require(regs && regs.size() == 9, "register map missing");
  for (unsigned i = 0; i < 9; ++i) {
    auto reg = cast<DictionaryAttr>(regs[i]);
    require(reg.getAs<StringAttr>("name") == names[i] && reg.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
        reg.getAs<BoolAttr>("readable").getValue() && reg.getAs<BoolAttr>("writeable").getValue(), "register map differs");
  }
  std::map<std::string, InstanceOp> instances;
  for (auto inst : top.getOps<InstanceOp>()) instances.emplace(inst.getName().str(), inst);
  auto sim = instances.at("sim"), mmio = instances.at("tsiRegisters");
  auto bulk = [&](Value dest, Value src) {
    for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest() == dest && conn.getSrc() == src) return true;
    return false;
  };
  require(bulk(sim.getResult(2), mmio.getResult(2)) && bulk(mmio.getResult(3), sim.getResult(3)) &&
      bulk(mmio.getResult(4), sim.getResult(4)) && bulk(top.getBodyBlock()->getArgument(3), mmio.getResult(5)),
      "TSI queue/scheduler/MCR wiring differs");
  for (unsigned i = 0; i < 2; ++i) {
    bool connected = false;
    for (auto conn : top.getOps<StrictConnectOp>()) connected |= conn.getDest() == mmio.getResult(i) && conn.getSrc() == top.getBodyBlock()->getArgument(i);
    require(connected, "bank must use host clock/reset");
  }
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGTSIMMIOWrapper|GGTSIWordQueuesWrapper>tsi_in_enq.bits",
      "~GGTSIMMIOWrapper|GGTSIWordQueuesWrapper>tsi_out_deq.valid",
      "~GGTSIMMIOWrapper|GGTSIWordQueuesWrapper>tsi_control.done",
      "~GGTSIMMIOWrapper|GGTSIMMIOWrapper>other", "~GGTSIMMIOWrapper|GGTSIMMIOWrapper>hostReset"};
  require(annos && annos.size() == 5, "annotations disappeared");
  for (unsigned i = 0; i < 5; ++i)
    require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target") == targets[i], "copied/internal annotation identity differs");
  require(succeeded(verify(*root)), "MMIO wrapper IR verification failed");
  std::string secondError;
  require(failed(goldengate::addTSIMMIOBank(c, secondError)), "repeated MMIO pass accepted");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 9; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGTSIWordQueuesWrapper"); OpBuilder b(&context);
    if (bad == 0) c.setName("WrongTop");
    if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2 || bad == 5 || bad >= 6) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      unsigned port = bad == 2 ? 1 : bad == 5 ? 5 : bad - 4;
      names[port] = b.getStringAttr(bad == 5 ? "tsiBridge_mcr" : "MissingPort"); top.setPortNames(names);
    }
    if (bad == 3) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 4) {
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGTSIMMIOBank"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addTSIMMIOBank(c, error)), "invalid TSI MMIO boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejected mapping mutated IR");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    phases(context); behavior(context); mapping(context); rejection(context);
    llvm::outs() << "TSI MMIO: 49,152 sample/write/reset transitions, readback, retriggered pulses, queue/scheduler wiring, register map and atomic rejection passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
