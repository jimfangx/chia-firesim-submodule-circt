// See LICENSE for license details.
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/ControlTransactionTracker.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef s) { if (!ok) throw std::runtime_error(s.str()); }
std::string dump(ModuleOp m) { std::string s; llvm::raw_string_ostream o(s); m.print(o); return s; }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == n) return m;
  throw std::runtime_error("missing module");
}
constexpr const char *oldTop = "GGControlWriteArbiterWrapper", *newTop = "GGControlWriteTrackerWrapper";
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0, unsigned slaveCount = 11) {
  const unsigned routeWidth = llvm::Log2_64_Ceil(slaveCount + 1);
  std::string s = "module { firrtl.circuit \"" + std::string(oldTop) + "\" { firrtl.module @" + oldTop + "(";
  struct P { const char *name; const char *type; const char *dir; };
  const P ports[]{{"hostClock", "clock", "in"}, {"hostReset", "uint<1>", "in"},
      {"ctrl_write_route_aw_tracker_ready", "uint<1>", "in"}, {"ctrl_write_route_aw_track_valid", "uint<1>", "out"},
      {"ctrl_write_dispatch_master_aw_bits_id", "uint<12>", "in"}, {"ctrl_decode_aw_target", "uint<4>", "out"}};
  bool first = true;
  for (auto p : ports) {
    if (bad == 1 && StringRef(p.name) == "ctrl_write_dispatch_master_aw_bits_id") continue;
    if (!first) s += ", "; first = false;
    s += std::string(bad == 3 && StringRef(p.name) == "ctrl_write_route_aw_tracker_ready" ? "out" : p.dir) + " %" + p.name + ": !firrtl." +
        (bad == 2 && StringRef(p.name) == "ctrl_write_dispatch_master_aw_bits_id" ? "uint<11>" :
         StringRef(p.name) == "ctrl_decode_aw_target" ? "uint<" + std::to_string(bad == 13 ? routeWidth + 1 : routeWidth) + ">" : p.type);
  }
  for (unsigned i = 0; i < slaveCount + 1; ++i) for (auto suffix : {"valid", "tag"}) {
    if (bad == 6 && i == slaveCount && StringRef(suffix) == "valid") continue;
    s += ", out %ctrl_write_tracker_deq_" + std::to_string(i) + "_" + suffix + ": !firrtl.uint<" + (StringRef(suffix) == "tag" ? "12" : "1") + ">";
  }
  if (bad == 4) s += ", out %ctrl_write_tracker_deq_0_data: !firrtl.uint<4>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> annos;
  for (auto ref : std::vector<std::string>{"hostReset", "ctrl_write_route_aw_track_valid", "ctrl_write_tracker_deq_" + std::to_string(slaveCount) + "_tag", "ctrl_write_dispatch_master_aw_bits_id"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Target")),
        b.getNamedAttr("target", b.getStringAttr("~" + std::string(oldTop) + "|" + oldTop + ">" + ref))}));
  if (bad != 10) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto decoder = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGControlAddressDecode"),
        ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    SmallVector<Attribute> regions;
    for (unsigned i = 0; i < (bad == 11 ? 0 : bad == 12 ? 64 : slaveCount); ++i)
      regions.push_back(b.getDictionaryAttr({b.getNamedAttr("name", b.getStringAttr("slave" + std::to_string(i)))}));
    decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(regions));
  }
  if (bad != 7) c->setAttr("rawAnnotations", b.getArrayAttr(annos));
  if (bad == 8) c.setNameAttr(b.getStringAttr("WrongTop"));
  if (bad == 5 || bad == 9) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto p = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(bad == 5 ? "GGControlWriteTracker" : "Parent"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    if (bad == 9) { b.setInsertionPointToStart(p.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), named(c, oldTop), "used"); }
  }
  return root;
}
struct Interpreter {
  FModuleOp module; std::map<std::string, Value> drivers; std::map<std::string, uint64_t> memo;
  std::map<std::string, Value> regs;
  std::string key(Value v) {
    if (auto a = dyn_cast<BlockArgument>(v)) return module.getPortName(a.getArgNumber()).str();
    if (auto r = v.getDefiningOp<RegOp>()) return r.getName().str();
    if (auto r = v.getDefiningOp<RegResetOp>()) return r.getName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto x : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(x.getDest()), x.getSrc()).second, "multiple drivers");
    for (auto r : m.getOps<RegOp>()) regs[key(r.getResult())] = r.getResult();
    for (auto r : m.getOps<RegResetOp>()) regs[key(r.getResult())] = r.getResult();
  }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k); auto *op = v.getDefiningOp(); uint64_t n;
    if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<OrPrimOp>(op)) n = eval(op->getOperand(0)) | eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bit = dyn_cast_or_null<BitsPrimOp>(op)) n = eval(bit.getInput()) >> bit.getLo();
    else throw std::runtime_error("unsupported expression");
    unsigned w = cast<UIntType>(v.getType()).getWidth().value(); return memo[k] = n & ((1ULL << w) - 1);
  }
  uint64_t get(std::string n) { return eval(drivers.at(n)); }
};
void test(MLIRContext &ctx, unsigned slaveCount) {
  const unsigned deqCount = slaveCount + 1, routeWidth = llvm::Log2_64_Ceil(deqCount);
  const unsigned routeSpace = 1u << routeWidth;
  auto root = fixture(ctx, 0, slaveCount); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlWriteTracker(c, error)), error);
  require(succeeded(verify(*root)), "invalid tracker IR");
  auto helper = named(c, "GGControlWriteTracker"), top = named(c, newTop);
  require(helper.getNumPorts() == 6 + 4 * deqCount && top.getNumPorts() == 4 + 2 * deqCount, "incorrect tracker boundary");
  require(helper->getAttrOfType<IntegerAttr>("goldengate.trackerDequeuePorts").getInt() == deqCount &&
      helper->getAttrOfType<IntegerAttr>("goldengate.trackerRouteWidth").getInt() == routeWidth,
      "tracker catalog geometry differs");
  for (auto p : helper.getPorts()) if (p.name.getValue() == "enq_bits_data" || p.name.getValue().ends_with("_data"))
    require(cast<UIntType>(p.type).getWidth() == routeWidth, "route port width differs");
  Interpreter sim(helper); require(sim.regs.size() == 192, "missing data/tag/free slots");
  for (auto [n, v] : sim.regs) {
    if (n.rfind("roq_data_", 0) == 0)
      require(cast<UIntType>(v.getType()).getWidth() == routeWidth && v.getDefiningOp<RegOp>(), "route state width/reset differs");
    if (n.rfind("roq_tags_", 0) == 0)
      require(cast<UIntType>(v.getType()).getWidth() == 6 && v.getDefiningOp<RegOp>(), "upper tag width/reset differs");
    if (n.rfind("roq_free_", 0) == 0)
      require(bool(v.getDefiningOp<RegResetOp>()), "occupancy lacks reset");
  }
  // Check the wrapper actually binds all normal/error retirements, rather than
  // merely exercising an otherwise correct but disconnected helper.
  llvm::DenseMap<Value, Value> connections;
  for (auto x : top.getOps<StrictConnectOp>()) connections[x.getDest()] = x.getSrc();
  InstanceOp tracker, inner;
  for (auto x : top.getOps<InstanceOp>()) {
    if (x.getName() == "controlWriteTracker") tracker = x;
    if (x.getName() == "sim") inner = x;
  }
  auto result = [](InstanceOp instance, StringRef port) -> Value {
    for (auto [i, name] : llvm::enumerate(instance.getPortNames()))
      if (cast<StringAttr>(name).getValue() == port) return instance.getResult(i);
    throw std::runtime_error("missing instance port");
  };
  for (unsigned i = 0; i < deqCount; ++i) for (auto suffix : {"valid", "tag"}) {
    std::string n = "deq_" + std::to_string(i) + "_" + suffix;
    require(connections.lookup(result(tracker, n)) == result(inner, "ctrl_write_tracker_" + n),
        "retirement connected to wrong slave");
  }
  require(connections.lookup(result(tracker, "enq_bits_data")) == result(inner, "ctrl_decode_aw_target"),
      "tracker did not bind decoded route");
  SmallVector<AssertOp> assertions(helper.getOps<AssertOp>());
  require(assertions.size() == deqCount, "missing retirement assertions");
  for (auto [i, a] : llvm::enumerate(assertions)) {
    require(a.getClock() == helper.getBodyBlock()->getArgument(0) &&
        a.getEventControl() == EventControl::AtPosEdge && !a.getIsConcurrent(),
        "retirement assertion samples the wrong edge");
    require(a.getMessage() == "aw_queue " + std::to_string(i) +
        " tried to dequeue untracked transaction", "retirement diagnostic identity differs");
  }
  std::vector<unsigned> detected(deqCount), suppressed(deqCount), matched(deqCount);
  std::mt19937_64 rng(184); std::array<unsigned, 64> free, tags, data;
  for (unsigned i = 0; i < 64; ++i) { free[i] = 1; tags[i] = rng() % 64; data[i] = rng() % routeSpace; }
  unsigned cases = 0;
  auto sample = [&](bool reset, bool enq, unsigned tag, unsigned payload,
                    const std::vector<unsigned> &deq, const std::vector<unsigned> &ids) {
    payload %= routeSpace;
    sim.memo = {{"reset", reset}, {"enq_valid", enq}, {"enq_bits_tag", tag}, {"enq_bits_data", payload}};
    for (unsigned i = 0; i < 64; ++i) {
      auto n = std::to_string(i); sim.memo["roq_free_" + n] = free[i]; sim.memo["roq_tags_" + n] = tags[i]; sim.memo["roq_data_" + n] = data[i];
    }
    for (unsigned i = 0; i < slaveCount + 1; ++i) {
      auto p = "deq_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = deq[i]; sim.memo[p + "tag"] = ids[i];
    }
    bool ready = free[tag % 64]; require(sim.get("enq_ready") == ready, "tag alias capacity differs");
    for (unsigned i = 0; i < slaveCount + 1; ++i) {
      auto p = "deq_" + std::to_string(i) + "_"; unsigned slot = ids[i] % 64;
      require(sim.get(p + "matches") == bool(!free[slot] && tags[slot] == ids[i] / 64), "matches gated or tag differs");
      require(sim.get(p + "data") == data[slot], "stored route payload differs");
      bool match = !free[slot] && tags[slot] == ids[i] / 64;
      auto a = assertions[i];
      require(sim.eval(a.getEnable()) == !reset, "reset did not mask assertion");
      require(sim.eval(a.getPredicate()) == (!deq[i] || match),
          "retirement assertion does not sample pre-edge tracking state");
      detected[i] += !reset && deq[i] && !match;
      suppressed[i] += reset && deq[i] && !match;
      matched[i] += !reset && deq[i] && match;
    }
    auto nf = free, nt = tags, nd = data;
    if (enq && ready) { unsigned slot = tag % 64; nf[slot] = 0; nt[slot] = tag / 64; nd[slot] = payload; }
    for (unsigned i = 0; i < slaveCount + 1; ++i) if (deq[i]) nf[ids[i] % 64] = 1;
    if (reset) nf.fill(1);
    for (auto [n, v] : sim.regs) {
      uint64_t next = sim.eval(sim.drivers.at(n));
      if (auto r = v.getDefiningOp<RegResetOp>()) if (reset) next = 1;
      unsigned slot = std::stoul(n.substr(n.rfind('_') + 1));
      unsigned expected = n.rfind("roq_free_", 0) == 0 ? nf[slot] : n.rfind("roq_tags_", 0) == 0 ? nt[slot] : nd[slot];
      require(next == expected, "tracker next state differs from enqueue/dequeue/reset priority");
    }
    free = nf; tags = nt; data = nd; ++cases;
  };
  std::vector<unsigned> deq(deqCount), ids(deqCount);
  for (unsigned i = 0; i < 64; ++i) sample(false, true, i + 64 * 13, i % deqCount, deq, ids);
  require(std::all_of(free.begin(), free.end(), [](unsigned x) { return !x; }), "full tracker accepted extra tags");
  for (unsigned i = 0; i < 64; ++i) {
    std::fill(ids.begin(), ids.end(), i + 64 * 14); sample(false, true, i + 64 * 14, 7, deq, ids); // stalled alias, matches false
    std::fill(deq.begin(), deq.end(), 1); sample(false, true, i + 64 * 14, 7, deq, ids); // mismatched retirement still frees
    sample(false, true, i + 64 * 14, 7, deq, ids); // put+retire: tag/data written, free wins
    std::fill(deq.begin(), deq.end(), 0); sample(false, true, i + 64 * 14, 7, deq, ids); // occupy same slot
  }
  sample(true, true, 4095, slaveCount, deq, ids);
  require(std::all_of(free.begin(), free.end(), [](unsigned x) { return x; }), "reset did not free every slot");
  sample(true, true, 4095, 5, deq, ids); // tag/data still write during reset
  for (unsigned n = 0; n < (slaveCount == 11 ? 1500u : 200u); ++n) {
    for (unsigned i = 0; i < slaveCount + 1; ++i) { deq[i] = (rng() % 8) == 0; ids[i] = rng() % 4096; }
    sample(n % 97 == 0, rng() & 1, rng() % 4096, rng() % routeSpace, deq, ids);
  }
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  // Exercise every retirement check together, including reset and an enqueue whose
  // tag matches a response at the same edge while the slot is still free.
  std::fill(deq.begin(), deq.end(), 1); std::fill(ids.begin(), ids.end(), 4095); sample(true, false, 4095, 0, deq, ids);
  std::fill(deq.begin(), deq.end(), 0); sample(false, true, 4095, 7, deq, ids);
  std::fill(deq.begin(), deq.end(), 1); sample(false, false, 4095, 0, deq, ids);
  sample(false, true, 4095, 9, deq, ids);
  sample(true, true, 4095, 5, deq, ids);
  for (unsigned j = 0; j < deqCount; ++j)
    require(detected[j] && suppressed[j] && matched[j],
        "missing failing, reset-suppressed or matched retirement coverage");
  for (auto a : annos) {
    auto target = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = target.drop_front(target.find('>'));
    require(target == "~" + std::string(newTop) + "|" + ((i == 1 || i == 2) ? oldTop : newTop) + suffix.str(), "consumed target transferred incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlWriteTracker(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  for (unsigned bad = 1; bad <= 13; ++bad) {
    auto r = fixture(ctx, bad, slaveCount); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlWriteTracker(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR");
  }
  llvm::outs() << "Control write tracker: " << cases << " collision/retirement/reset cycles for " << slaveCount
      << " slaves, " << deqCount << " assertions, route width " << routeWidth
      << ", targets and fourteen atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    for (unsigned count : {1u, 2u, 3u, 11u, 13u, 31u, 63u}) test(ctx, count);
    auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin();
    auto before = dump(*root);
    for (unsigned count : {0u, 64u, ~0u}) {
      require(goldengate::controlTransactionTrackerPorts(&ctx, count).empty(), "invalid helper ports accepted");
      require(!goldengate::createControlTransactionTracker(c, "InvalidTracker", "aw_queue", count), "invalid helper count accepted");
      require(dump(*root) == before, "invalid helper count mutated circuit");
    }
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
