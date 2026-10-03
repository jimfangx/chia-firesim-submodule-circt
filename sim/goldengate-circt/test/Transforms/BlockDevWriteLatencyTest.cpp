// See LICENSE for license details.
#include "goldengate/BlockDevWriteLatency.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef why) { if (!ok) throw std::runtime_error(why.str()); }
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    for (auto c : m.getOps<ConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt read(std::string k, unsigned w) {
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    if (drivers.count(k)) return eval(drivers.at(k)).zextOrTrunc(w);
    auto prefix = k;
    while (prefix.find('.') != std::string::npos) {
      prefix.resize(prefix.rfind('.'));
      if (drivers.count(prefix)) return read(key(drivers.at(prefix)) + k.substr(prefix.size()), w);
    }
    throw std::runtime_error("missing aggregate driver: " + k);
  }
  APInt output(Value v, llvm::StringRef path, unsigned w = 64) {
    return read(key(v) + (path.empty() ? "" : "." + path.str()), w);
  }
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
    else if (isa_and_nonnull<NEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) != eval(op->getOperand(1)));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ult(eval(op->getOperand(1))));
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};

OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode = 0) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGBlockDevBridgeBoundWrapper\" { firrtl.module @GGBlockDevBridgeBoundWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); }; auto bit = uint(1);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> fields) { return BundleType::get(&ctx, fields); };
  auto latency = bundle({{b.getStringAttr("read_latency"), false, uint(24)}, {b.getStringAttr("write_latency"), false, uint(24)}});
  auto timing = bundle({{b.getStringAttr("returnWrite"), false, bit}, {b.getStringAttr("readRespBusy"), false, bit},
      {b.getStringAttr("wAckStallN"), true, bit}, {b.getStringAttr("rRespStallN"), true, bit}, {b.getStringAttr("tCycle"), true, uint(24)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In},
      {b.getStringAttr("blockdev_queue_reset"), bit, Direction::Out},
      {b.getStringAttr("blockdev_write_latency_enq_valid"), bit, Direction::Out},
      {b.getStringAttr("blockdev_latency"), latency, Direction::Out},
      {b.getStringAttr("blockdev_timing"), timing, Direction::In},
      {b.getStringAttr("other"), uint(8), Direction::Out}};
  if (mode >= 7 && mode < 13) ports[mode - 7].name = b.getStringAttr("missing");
  if (mode >= 13 && mode < 19) {
    auto &port = ports[mode - 13]; port.direction = port.direction == Direction::In ? Direction::Out : Direction::In;
  }
  if (mode >= 19 && mode < 25) ports[mode - 19].type = uint(7);
  if (mode == 25 || mode == 26) ports[6].name = b.getStringAttr(mode == 25 ? "blockdev_write_latency_deq" : "blockdev_write_latency_enq_ready");
  auto top = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  SmallVector<Attribute> annotations;
  for (auto target : {"hostClock", "hostReset", "blockdev_queue_reset", "blockdev_write_latency_enq_valid",
                     "blockdev_latency.write_latency", "blockdev_timing.tCycle", "other"})
    annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGBlockDevBridgeBoundWrapper|GGBlockDevBridgeBoundWrapper>" + std::string(target))),
        b.getNamedAttr("retainedMetadata", b.getI32IntegerAttr(204))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (mode == 1) c.setName("UnexpectedTop");
  if (mode == 2) c->removeAttr("rawAnnotations");
  if (mode == 3) top.erase();
  if (mode == 4 || mode == 5)
    b.create<FModuleOp>(c.getLoc(), b.getStringAttr(mode == 4 ? "GGBlockDevWriteLatencyWrapper" : "GGBlockDevWriteLatencyPipe"),
        ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
  if (mode == 6) {
    auto user = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("User"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "top");
  }
  return root;
}
// The reference keeps a one-slot logical queue plus a due-cycle and pending
// flag. It does not evaluate the implementation's expressions or next drivers.
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevWriteLatency(c, error)), error);
  require(succeeded(verify(*root)), "invalid write latency IR");
  Interpreter sim(named(c, "GGBlockDevWriteLatencyPipe"));
  RegResetOp full, pending; RegOp deadline;
  for (auto r : sim.module.getOps<RegResetOp>()) {
    if (r.getName() == "maybe_full") full = r;
    if (r.getName() == "pendingRegisters_0") pending = r;
  }
  for (auto r : sim.module.getOps<RegOp>()) deadline = r;
  require(full && pending && deadline && deadline.getName() == "latencies_0", "missing latency queue state");
  require(std::distance(sim.module.getOps<RegResetOp>().begin(), sim.module.getOps<RegResetOp>().end()) == 2 &&
      std::distance(sim.module.getOps<RegOp>().begin(), sim.module.getOps<RegOp>().end()) == 1 &&
      sim.width(full.getResult()) == 1 && sim.width(pending.getResult()) == 1 && sim.width(deadline.getResult()) == 24,
      "incorrect state geometry/reset");
  require(std::distance(sim.module.getOps<AssertOp>().begin(), sim.module.getOps<AssertOp>().end()) == 1, "missing latency assertion");
  auto assertion = *sim.module.getOps<AssertOp>().begin();
  bool occupied = false, waiting = false; uint32_t due = 0;
  unsigned samples = 0, blocked = 0, resetWrites = 0, emptyMatch = 0, enqueueMatch = 0, wraps = 0, lateRelease = 0, zeroFailures = 0;
  auto seed = [&](bool hasToken, bool pendingDeadline, uint32_t deadlineCycle) {
    occupied = hasToken; waiting = pendingDeadline; due = deadlineCycle;
    sim.state[full.getResult()] = APInt(1, occupied); sim.state[pending.getResult()] = APInt(1, waiting);
    sim.state[deadline.getResult()] = APInt(24, due);
  };
  auto check = [&](bool reset, bool enqValid, bool deqReady, uint32_t latency, uint32_t cycle) {
    sim.memo.clear(); sim.put(sim.arg(1), "", reset); sim.put(sim.arg(2), "", enqValid);
    sim.put(sim.arg(4), "", deqReady); sim.put(sim.arg(6), "", latency); sim.put(sim.arg(7), "", cycle);
    bool ready = !occupied, valid = occupied && (!waiting || due == cycle);
    bool push = enqValid && ready, pop = deqReady && valid;
    require(sim.output(sim.arg(3), "").getBoolValue() == ready, "enqueue ready differs (no flow/pipe/full replacement)");
    require(sim.output(sim.arg(5), "").getBoolValue() == valid, "release differs at matching or passed deadline");
    require(sim.eval(assertion.getEnable()).getBoolValue() == !reset &&
        sim.eval(assertion.getPredicate()).getBoolValue() == (!push || latency != 0), "latency assertion gating differs");
    bool nextOccupied = push != pop ? push : occupied;
    bool nextWaiting = due == cycle ? false : waiting;
    uint32_t nextDue = due;
    if (push) { nextWaiting = latency != 1; nextDue = (cycle + latency) & 0xffffffu; }
    if (reset) nextOccupied = nextWaiting = false;
    blocked += enqValid && !ready; resetWrites += reset && push;
    emptyMatch += !occupied && due == cycle; enqueueMatch += push && due == cycle;
    wraps += push && uint64_t(cycle) + latency > 0xffffffu;
    lateRelease += valid && due != cycle; zeroFailures += !reset && push && latency == 0;
    sim.edge();
    require(sim.state.lookup(full.getResult()).getBoolValue() == nextOccupied, "occupancy enqueue/dequeue/reset differs");
    require(sim.state.lookup(pending.getResult()).getBoolValue() == nextWaiting, "pending match/enqueue/reset priority differs");
    require(sim.state.lookup(deadline.getResult()).getZExtValue() == nextDue, "unreset deadline load/hold/wrap differs");
    occupied = nextOccupied; waiting = nextWaiting; due = nextDue; ++samples;
  };
  for (bool hasToken : {false, true}) for (bool isPending : {false, true})
    for (uint32_t dueCycle : {0u, 1u, 0xfffffeu, 0xffffffu, 0x123456u})
      for (uint32_t cycle : {0u, 1u, 0xfffffeu, 0xffffffu})
        for (uint32_t latency : {0u, 1u, 2u, 0xfffffeu, 0xffffffu})
          for (unsigned flags = 0; flags < 8; ++flags) {
            seed(hasToken, isPending, dueCycle); check(flags & 1, flags & 2, flags & 4, latency, cycle);
          }
  // Wrap through zero, observe the due cycle while stalled, then move beyond
  // it before accepting. Repeated host edges at a frozen target cycle matter.
  seed(false, false, 0); check(false, true, true, 5, 0xfffffdu);
  for (unsigned n = 0; n < 5; ++n) check(false, true, true, 9, 0xfffffdu);
  check(false, true, true, 9, 0); check(false, false, false, 9, 1);
  check(false, true, false, 9, 2); check(false, true, false, 9, 2);
  check(false, true, false, 9, 3); check(false, true, true, 9, 3);
  require(!occupied, "deadline passed while response stalled was lost");
  check(false, true, true, 1, 3); check(false, true, true, 7, 3);
  require(!occupied, "latency one fast path or blocked simultaneous replacement differs");
  // Zero latency asserts on accepted input only; reset masks the assertion but
  // still writes the unreset deadline, even with a matching old deadline.
  seed(false, true, 7); check(true, true, false, 0, 7);
  require(!occupied && !waiting && due == 7, "reset priority/deadline retention differs");
  std::mt19937_64 rng(204);
  for (unsigned n = 0; n < 20000; ++n) {
    unsigned flags = rng(); uint32_t latency = n % 4 ? (rng() & 0xffffffu) : n % 3;
    uint32_t cycle = n % 4 ? (rng() & 0xffffffu) : due;
    check(flags & 1, flags & 2, flags & 4, latency, cycle);
  }
  require(blocked && resetWrites && emptyMatch && enqueueMatch && wraps && lateRelease && zeroFailures,
      "missing latency/reset/deadline/priority/assertion coverage");
  llvm::outs() << samples << " write latency state/handshake/assertion comparisons passed; "
      << resetWrites << " reset-time deadline writes, " << enqueueMatch << " matching-cycle enqueues\n";
}
std::string print(ModuleOp root) { std::string s; llvm::raw_string_ostream out(s); root.print(out); return s; }
void wiringAndAnnotations(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations"); auto old = named(c, "GGBlockDevBridgeBoundWrapper");
  auto oldPorts = old.getPorts();
  require(succeeded(goldengate::addBlockDevWriteLatency(c, error)), error);
  require(succeeded(verify(*root)), "invalid latency wrapper");
  auto wrapper = named(c, "GGBlockDevWriteLatencyWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 8, "wrong consumed/copied/exposed ports");
  const unsigned copied[]{0, 1, 2, 4, 5, 6};
  for (unsigned i = 0; i < 6; ++i) require(wrapper.getPortName(i) == oldPorts[copied[i]].name &&
      wrapper.getPortType(i) == oldPorts[copied[i]].type && wrapper.getPortDirection(i) == oldPorts[copied[i]].direction,
      "copied port order/type/direction changed");
  require(wrapper.getPortName(6) == "blockdev_write_latency_deq" && wrapper.getPortDirection(6) == Direction::In &&
      wrapper.getPortName(7) == "blockdev_write_latency_enq_ready" && wrapper.getPortDirection(7) == Direction::Out,
      "missing latency dequeue/readiness boundary");
  auto deqType = dyn_cast<BundleType>(wrapper.getPortType(6));
  require(deqType && deqType.getNumElements() == 2 && deqType.getElements()[0].name == "ready" && !deqType.getElements()[0].isFlip &&
      deqType.getElements()[1].name == "valid" && deqType.getElements()[1].isFlip, "dequeue boundary flips differ");
  InstanceOp inner, pipe;
  for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getModuleName() == "GGBlockDevWriteLatencyPipe") pipe = i; else inner = i; }
  require(inner && pipe, "missing inner/latency instances");
  require(sim.drivers.at(sim.key(pipe.getResult(0))) == sim.arg(0), "host clock binding differs");
  sim.put(inner.getResult(2), "", 1); require(sim.output(pipe.getResult(1), "").getBoolValue(), "qualified queue reset not bound");
  sim.put(inner.getResult(3), "", 1); require(sim.output(pipe.getResult(2), "").getBoolValue(), "completion pulse not bound");
  sim.put(inner.getResult(4), "write_latency", 0xabcdef);
  require(sim.output(pipe.getResult(6), "").getZExtValue() == 0xabcdef, "programmed latency not bound");
  sim.put(inner.getResult(5), "tCycle", 0xfedcba);
  require(sim.output(pipe.getResult(7), "").getZExtValue() == 0xfedcba, "target cycle not bound");
  sim.put(sim.arg(6), "ready", 1); require(sim.output(pipe.getResult(4), "").getBoolValue(), "dequeue acceptance not bound");
  sim.put(pipe.getResult(5), "", 1); require(sim.output(sim.arg(6), "valid").getBoolValue(), "dequeue valid not bound");
  sim.put(pipe.getResult(3), "", 1); require(sim.output(sim.arg(7), "").getBoolValue(), "enqueue readiness not exposed");
  auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(after.size() == before.size(), "annotation loss");
  for (unsigned i = 0; i < after.size(); ++i) {
    auto oldAnno = cast<DictionaryAttr>(before[i]), newAnno = cast<DictionaryAttr>(after[i]);
    require(oldAnno.get("class") == newAnno.get("class") && oldAnno.get("retainedMetadata") == newAnno.get("retainedMetadata"), "annotation metadata changed");
    auto target = goldengate::resolveAnnotationTarget(c, newAnno.getAs<StringAttr>("target").getValue(), error);
    require(target && target->module.getModuleName() == (i == 3 ? "GGBlockDevBridgeBoundWrapper" : "GGBlockDevWriteLatencyWrapper"), "consumed/copied annotation target transfer differs");
  }
  llvm::outs() << "eight wrapper bindings and seven annotation transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  std::string error;
  for (unsigned mode = 1; mode <= 26; ++mode) {
    auto bad = fixture(ctx, mode); auto c = *bad->getOps<CircuitOp>().begin(); auto before = print(*bad);
    require(failed(goldengate::addBlockDevWriteLatency(c, error)) && !error.empty() && before == print(*bad), "invalid boundary accepted or mutated");
  }
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::addBlockDevWriteLatency(c, error)), error); auto before = print(*root);
  require(failed(goldengate::addBlockDevWriteLatency(c, error)) && !error.empty() && before == print(*root), "repeat changed IR");
  llvm::outs() << "26 malformed boundaries and repeated-pass atomic rejection passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); wiringAndAnnotations(ctx); rejection(ctx); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
