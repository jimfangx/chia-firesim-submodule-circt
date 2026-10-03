// See LICENSE for license details.
#include "goldengate/BlockDevResponseScheduler.h"
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
#include <tuple>
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
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGBlockDevReadLatencyWrapper\" { firrtl.module @GGBlockDevReadLatencyWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); }; auto bit = uint(1);
  auto bundle = [&](std::initializer_list<BundleType::BundleElement> f) { return BundleType::get(&ctx, f); };
  auto timing = bundle({{b.getStringAttr("returnWrite"), false, bit}, {b.getStringAttr("readRespBusy"), false, bit},
      {b.getStringAttr("wAckStallN"), true, bit}, {b.getStringAttr("rRespStallN"), true, bit}, {b.getStringAttr("tCycle"), true, uint(24)}});
  auto write = bundle({{b.getStringAttr("ready"), false, bit}, {b.getStringAttr("valid"), true, bit}});
  auto read = bundle({{b.getStringAttr("ready"), false, bit}, {b.getStringAttr("valid"), true, bit}, {b.getStringAttr("bits"), true, uint(32)}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("blockdev_queue_reset"), bit, Direction::Out}, {b.getStringAttr("blockdev_tfire"), bit, Direction::Out},
      {b.getStringAttr("blockdev_resp_ready"), bit, Direction::Out}, {b.getStringAttr("blockdev_timing"), timing, Direction::In},
      {b.getStringAttr("blockdev_write_latency_deq"), write, Direction::In}, {b.getStringAttr("blockdev_read_latency_deq"), read, Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}, {b.getStringAttr("other"), uint(8), Direction::Out}};
  if (mode >= 7 && mode < 14) ports[mode - 7].name = b.getStringAttr("missing");
  if (mode >= 14 && mode < 21) {
    auto &p = ports[mode - 14]; p.direction = p.direction == Direction::In ? Direction::Out : Direction::In;
  }
  if (mode >= 21 && mode < 28) ports[mode - 21].type = uint(7);
  auto top = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  SmallVector<Attribute> annotations;
  for (auto port : ports) annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target", b.getStringAttr("~GGBlockDevReadLatencyWrapper|GGBlockDevReadLatencyWrapper>" + port.name.getValue().str())),
      b.getNamedAttr("metadata", b.getI32IntegerAttr(206))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  if (mode == 1) c.setName("WrongTop");
  if (mode == 2) c->removeAttr("rawAnnotations");
  if (mode == 3) top.erase();
  if (mode == 4 || mode == 5) b.create<FModuleOp>(c.getLoc(), b.getStringAttr(mode == 4 ? "GGBlockDevResponseScheduler" : "GGBlockDevResponseSchedulerWrapper"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
  if (mode == 6) {
    auto user = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("User"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "top");
  }
  return root;
}
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevResponseScheduler(c, error)), error);
  require(succeeded(verify(*root)), "invalid scheduler IR");
  Interpreter sim(named(c, "GGBlockDevResponseScheduler")); RegResetOp beats, returning;
  for (auto r : sim.module.getOps<RegResetOp>()) { if (r.getName() == "returnWrite") returning = r; else beats = r; }
  require(beats && returning && beats.getName() == "readRespBeatsLeft" && sim.width(beats.getResult()) == 32 && sim.width(returning.getResult()) == 1, "wrong scheduler state/reset");
  require(sim.state.size() == 2, "extra scheduler state");
  std::mt19937_64 rng(206); unsigned samples = 0, resets = 0, stalls = 0, priorities = 0, chains = 0, loads = 0, zeroLoads = 0;
  auto check = [&](uint32_t count, bool write, unsigned flags, uint32_t length) {
    bool reset = flags & 1, fire = flags & 2, ready = flags & 4, wvalid = flags & 8, rvalid = flags & 16;
    sim.state[beats.getResult()] = APInt(32, count); sim.state[returning.getResult()] = APInt(1, write); sim.memo.clear();
    for (auto [i, n] : {std::pair<unsigned, uint64_t>{1, reset}, {2, fire}, {3, ready}, {4, wvalid}, {5, rvalid}, {6, length}}) sim.put(sim.arg(i), "", n);
    bool active = write || count != 0, completed = ready && active && (write || count == 1);
    bool choose = completed || !active, takeWrite = fire && choose && wvalid, takeRead = fire && choose && !wvalid && rvalid;
    auto out = [&](unsigned i) { return sim.output(sim.arg(i), "").getZExtValue(); };
    require(out(7) == takeWrite && out(8) == takeRead && out(9) == write && out(10) == (count != 0), "scheduler output differs");
    uint32_t nextCount = count; bool nextWrite = write;
    if (fire) {
      if (choose) { nextCount = 0; nextWrite = false;
        if (wvalid) nextWrite = true;
        else if (rvalid) nextCount = uint64_t(length) * 64;
      } else if (count && ready) --nextCount;
    }
    if (reset) { nextCount = 0; nextWrite = false; }
    sim.edge();
    require(sim.state.lookup(beats.getResult()).getZExtValue() == nextCount && sim.state.lookup(returning.getResult()).getBoolValue() == nextWrite, "scheduler next state differs");
    ++samples; resets += reset && (takeWrite || takeRead); stalls += !fire && active;
    priorities += takeWrite && rvalid; chains += fire && completed && (wvalid || rvalid); loads += takeRead;
    zeroLoads += takeRead && !uint32_t(uint64_t(length) * 64);
  };
  for (uint32_t count : {0u, 1u, 2u, 63u, 64u, 0xffffu, 0x40000000u, 0xfffffffeu, 0xffffffffu})
    for (bool write : {false, true}) for (unsigned flags = 0; flags < 32; ++flags)
      for (uint32_t length : {0u, 1u, 0x3ffffffu, 0x4000000u, 0x4000001u, 0xffffffffu}) check(count, write, flags, length);
  for (unsigned n = 0; n < 10000; ++n) check(sim.state.lookup(beats.getResult()).getZExtValue(), sim.state.lookup(returning.getResult()).getBoolValue(), rng() % 32, rng());
  require(resets && stalls && priorities && chains && loads && zeroLoads, "missing scheduler boundary coverage");
  llvm::outs() << samples << " scheduler comparisons passed; " << resets << " reset-time dequeues, " << priorities << " write-priority collisions, " << chains << " chained responses\n";
}
std::string print(ModuleOp m) { std::string s; llvm::raw_string_ostream out(s); m.print(out); return s; }
void wiring(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations"); std::string error;
  auto old = named(c, "GGBlockDevReadLatencyWrapper"); auto ports = old.getPorts();
  require(succeeded(goldengate::addBlockDevResponseScheduler(c, error)) && succeeded(verify(*root)), "invalid scheduler wrapper");
  auto wrapper = named(c, "GGBlockDevResponseSchedulerWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 5, "incorrect consumed/copied ports");
  const unsigned copied[]{0, 1, 2, 7, 8};
  InstanceOp inner, scheduler;
  for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getModuleName() == "GGBlockDevResponseScheduler") scheduler = i; else inner = i; }
  require(inner && scheduler, "missing scheduler instances");
  for (unsigned j = 0; j < 5; ++j) {
    auto i = copied[j]; require(wrapper.getPorts()[j].name == ports[i].name && wrapper.getPorts()[j].type == ports[i].type && wrapper.getPorts()[j].direction == ports[i].direction, "copied port changed");
    Value dest = ports[i].direction == Direction::In ? inner.getResult(i) : sim.arg(j);
    require(sim.drivers.at(sim.key(dest)) == (ports[i].direction == Direction::In ? sim.arg(j) : inner.getResult(i)), "copied binding differs");
  }
  require(sim.drivers.at(sim.key(scheduler.getResult(0))) == sim.arg(0), "scheduler clock differs");
  for (unsigned i = 1; i <= 3; ++i) require(sim.drivers.at(sim.key(scheduler.getResult(i))) == inner.getResult(i), "scheduler reset/token/response input differs");
  for (auto [port, innerPort, field] : {std::tuple<unsigned, unsigned, const char *>{4, 5, "valid"}, {5, 6, "valid"}, {6, 6, "bits"}}) {
    sim.put(inner.getResult(innerPort), field, port == 6 ? 0xfedcba98 : 1);
    require(sim.output(scheduler.getResult(port), "").getZExtValue() == (port == 6 ? 0xfedcba98 : 1), "latency input differs");
  }
  for (auto [port, innerPort, field] : {std::tuple<unsigned, unsigned, const char *>{7, 5, "ready"}, {8, 6, "ready"}, {9, 4, "returnWrite"}, {10, 4, "readRespBusy"}}) {
    sim.put(scheduler.getResult(port), "", 1); require(sim.output(inner.getResult(innerPort), field).getBoolValue(), "scheduler output binding differs");
  }
  auto after = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(after.size() == before.size(), "annotation loss");
  for (unsigned i = 0; i < after.size(); ++i) {
    auto a = cast<DictionaryAttr>(after[i]), oldAnno = cast<DictionaryAttr>(before[i]);
    require(a.get("class") == oldAnno.get("class") && a.get("metadata") == oldAnno.get("metadata"), "annotation metadata changed");
    auto target = goldengate::resolveAnnotationTarget(c, a.getAs<StringAttr>("target").getValue(), error);
    require(target && target->module.getModuleName() == (i >= 3 && i <= 6 ? "GGBlockDevReadLatencyWrapper" : "GGBlockDevResponseSchedulerWrapper"), "annotation transfer differs");
  }
  llvm::outs() << "eleven scheduler bindings, five copied ports and nine annotation transfers passed\n";
}
void rejection(MLIRContext &ctx) {
  std::string error;
  for (unsigned mode = 1; mode <= 27; ++mode) {
    auto bad = fixture(ctx, mode); auto c = *bad->getOps<CircuitOp>().begin(); auto before = print(*bad);
    require(failed(goldengate::addBlockDevResponseScheduler(c, error)) && !error.empty() && before == print(*bad), "malformed boundary mutated");
  }
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
  require(succeeded(goldengate::addBlockDevResponseScheduler(c, error)), error); auto before = print(*root);
  require(failed(goldengate::addBlockDevResponseScheduler(c, error)) && before == print(*root), "repeated pass mutated");
  llvm::outs() << "27 malformed boundaries and repeated-pass atomic rejection passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); wiring(ctx); rejection(ctx); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
