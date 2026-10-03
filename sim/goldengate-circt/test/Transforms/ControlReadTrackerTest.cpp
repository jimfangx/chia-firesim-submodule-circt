// See LICENSE for license details.
#include "goldengate/ControlReadTracker.h"
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
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef s) { if (!ok) throw std::runtime_error(s.str()); }
std::string dump(ModuleOp m) { std::string s; llvm::raw_string_ostream o(s); m.print(o); return s; }
FModuleOp named(CircuitOp c, llvm::StringRef n) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == n) return m;
  throw std::runtime_error("missing module");
}
constexpr const char *oldTop = "GGControlReadDispatchWrapper", *newTop = "GGControlReadTrackerWrapper";
const std::pair<unsigned, const char *> widgets[]{{2, "tracerv_ctrl"}, {4, "loadmem_ctrl"},
    {5, "peekPokeBridge_ctrl"}, {6, "uartBridge_ctrl"}, {7, "clockBridge_ctrl"},
    {9, "resetBridge_ctrl"}, {10, "cpuStream_ctrl"}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto token = [](std::string s) { return "bundle<ready flip: uint<1>, valid: uint<1>, bits: " + s + ">"; };
  std::string control = "!firrtl.bundle<b flip: " + token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>") +
      ", r flip: " + token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>") + ">";
  std::string s = "module { firrtl.circuit \"" + std::string(oldTop) + "\" { firrtl.module @" + oldTop + "(";
  struct P { const char *name; const char *type; const char *dir; };
  const P ports[]{{"hostClock", "clock", "in"}, {"hostReset", "uint<1>", "in"},
      {"ctrl_read_dispatch_tracker_ready", "uint<1>", "in"}, {"ctrl_read_dispatch_track_valid", "uint<1>", "out"},
      {"ctrl_read_dispatch_track_tag", "uint<12>", "out"}, {"ctrl_read_dispatch_track_target", "uint<4>", "out"},
      {"ctrl_error_r_ready", "uint<1>", "in"}, {"ctrl_error_r_valid", "uint<1>", "out"},
      {"ctrl_error_r_bits_last", "uint<1>", "out"}, {"ctrl_error_r_bits_id", "uint<12>", "out"}};
  bool first = true;
  for (auto p : ports) {
    if (bad == 1 && StringRef(p.name) == "ctrl_read_dispatch_track_tag") continue;
    if (!first) s += ", "; first = false;
    s += std::string(p.dir) + " %" + p.name + ": !firrtl." +
        (bad == 2 && StringRef(p.name) == "ctrl_read_dispatch_track_tag" ? "uint<11>" : p.type);
  }
  for (auto [i, n] : widgets) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  if (bad == 4) s += ", in %ctrl_read_tracker_deq_0_valid: !firrtl.uint<1>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> rows, annos;
  for (auto [i, n] : widgets) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("port", b.getStringAttr(n)), b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 5 && i == 7 ? 6 : i))}));
  if (bad != 6) named(c, oldTop)->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
  for (auto ref : {"hostReset", "ctrl_read_dispatch_track_tag", "clockBridge_ctrl.r.bits.id"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Target")),
        b.getNamedAttr("target", b.getStringAttr("~" + std::string(oldTop) + "|" + oldTop + ">" + ref))}));
  if (bad != 7) c->setAttr("rawAnnotations", b.getArrayAttr(annos));
  if (bad == 8) c.setNameAttr(b.getStringAttr("WrongTop"));
  if (bad == 9) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto p = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Parent"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    b.setInsertionPointToStart(p.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), named(c, oldTop), "used");
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
void test(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlReadTracker(c, error)), error);
  require(succeeded(verify(*root)), "invalid tracker IR");
  auto helper = named(c, "GGControlReadTracker"), top = named(c, newTop);
  require(helper.getNumPorts() == 54 && top.getNumPorts() == 45, "incorrect tracker boundary");
  Interpreter sim(helper); require(sim.regs.size() == 192, "missing data/tag/free slots");
  SmallVector<AssertOp> assertions(helper.getOps<AssertOp>());
  require(assertions.size() == 12, "missing retirement assertions");
  for (auto [i, a] : llvm::enumerate(assertions)) {
    require(a.getClock() == helper.getBodyBlock()->getArgument(0) &&
        a.getEventControl() == EventControl::AtPosEdge && !a.getIsConcurrent(),
        "retirement assertion samples the wrong edge");
    require(a.getMessage() == "ar_queue " + std::to_string(i) +
        " tried to dequeue untracked transaction", "retirement diagnostic identity differs");
  }
  std::array<unsigned, 12> detected{}, suppressed{}, matched{};
  std::mt19937_64 rng(181); std::array<unsigned, 64> free, tags, data;
  for (unsigned i = 0; i < 64; ++i) { free[i] = 1; tags[i] = rng() % 64; data[i] = rng() % 16; }
  unsigned cases = 0;
  auto sample = [&](bool reset, bool enq, unsigned tag, unsigned payload,
                    const std::array<unsigned, 12> &deq, const std::array<unsigned, 12> &ids) {
    sim.memo = {{"reset", reset}, {"enq_valid", enq}, {"enq_bits_tag", tag}, {"enq_bits_data", payload}};
    for (unsigned i = 0; i < 64; ++i) {
      auto n = std::to_string(i); sim.memo["roq_free_" + n] = free[i]; sim.memo["roq_tags_" + n] = tags[i]; sim.memo["roq_data_" + n] = data[i];
    }
    for (unsigned i = 0; i < 12; ++i) {
      auto p = "deq_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = deq[i]; sim.memo[p + "tag"] = ids[i];
    }
    bool ready = free[tag % 64]; require(sim.get("enq_ready") == ready, "tag alias capacity differs");
    for (unsigned i = 0; i < 12; ++i) {
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
    for (unsigned i = 0; i < 12; ++i) if (deq[i]) nf[ids[i] % 64] = 1;
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
  std::array<unsigned, 12> deq{}, ids{};
  for (unsigned i = 0; i < 64; ++i) sample(false, true, i + 64 * 13, i % 12, deq, ids);
  require(std::all_of(free.begin(), free.end(), [](unsigned x) { return !x; }), "full tracker accepted extra tags");
  for (unsigned i = 0; i < 64; ++i) {
    ids.fill(i + 64 * 14); sample(false, true, i + 64 * 14, 7, deq, ids); // stalled alias, matches false
    deq.fill(1); sample(false, true, i + 64 * 14, 7, deq, ids); // mismatched retirement still frees
    sample(false, true, i + 64 * 14, 7, deq, ids); // put+retire: tag/data written, free wins
    deq.fill(0); sample(false, true, i + 64 * 14, 7, deq, ids); // occupy same slot
  }
  sample(true, true, 4095, 11, deq, ids);
  require(std::all_of(free.begin(), free.end(), [](unsigned x) { return x; }), "reset did not free every slot");
  sample(true, true, 4095, 5, deq, ids); // tag/data still write during reset
  for (unsigned n = 0; n < 1500; ++n) {
    for (unsigned i = 0; i < 12; ++i) { deq[i] = (rng() % 8) == 0; ids[i] = rng() % 4096; }
    sample(n % 97 == 0, rng() & 1, rng() % 4096, rng() % 16, deq, ids);
  }
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  // Exercise all twelve checks together, including reset and an enqueue whose
  // tag matches a response at the same edge while the slot is still free.
  deq.fill(1); ids.fill(4095); sample(true, false, 4095, 0, deq, ids);
  deq.fill(0); sample(false, true, 4095, 7, deq, ids);
  deq.fill(1); sample(false, false, 4095, 0, deq, ids);
  sample(false, true, 4095, 9, deq, ids);
  sample(true, true, 4095, 5, deq, ids);
  for (unsigned j = 0; j < 12; ++j)
    require(detected[j] && suppressed[j] && matched[j],
        "missing failing, reset-suppressed or matched retirement coverage");
  for (auto a : annos) {
    auto target = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = target.drop_front(target.find('>'));
    require(target == "~" + std::string(newTop) + "|" + (i == 1 ? oldTop : newTop) + suffix.str(), "consumed target transferred incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlReadTracker(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  for (unsigned bad = 1; bad <= 9; ++bad) {
    auto r = fixture(ctx, bad); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlReadTracker(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR");
  }
  llvm::outs() << "Control read tracker: " << cases << " collision/retirement/reset cycles, twelve retirement assertions, targets and ten atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
