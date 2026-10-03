// See LICENSE for license details.
#include "goldengate/ControlReadArbiter.h"
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
constexpr const char *oldTop = "GGControlReadTrackerWrapper", *newTop = "GGControlReadArbiterWrapper";
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
      {"ctrl_error_r_ready", "uint<1>", "in"}, {"ctrl_error_r_valid", "uint<1>", "out"},
      {"ctrl_error_r_bits_last", "uint<1>", "out"}, {"ctrl_error_r_bits_id", "uint<12>", "out"},
      {"ctrl_error_r_bits_data", "uint<32>", "out"}, {"ctrl_error_r_bits_resp", "uint<2>", "out"},
      {"ctrl_error_r_bits_user", "uint<1>", "out"},
      {"ctrl_read_tracker_deq_0_valid", "uint<1>", "in"}, {"ctrl_read_tracker_deq_0_tag", "uint<12>", "in"},
      {"ctrl_read_tracker_deq_1_valid", "uint<1>", "in"}, {"ctrl_read_tracker_deq_1_tag", "uint<12>", "in"},
      {"ctrl_read_tracker_deq_3_valid", "uint<1>", "in"}, {"ctrl_read_tracker_deq_3_tag", "uint<12>", "in"},
      {"ctrl_read_tracker_deq_8_valid", "uint<1>", "in"}, {"ctrl_read_tracker_deq_8_tag", "uint<12>", "in"}};
  bool first = true;
  for (auto p : ports) {
    if (bad == 1 && StringRef(p.name) == "ctrl_error_r_bits_id") continue;
    if (!first) s += ", "; first = false;
    s += std::string(p.dir) + " %" + p.name + ": !firrtl." +
        (bad == 2 && StringRef(p.name) == "ctrl_error_r_bits_id" ? "uint<11>" : p.type);
  }
  for (auto [i, n] : widgets) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  if (bad == 4) s += ", in %ctrl_read_arb_in_0_valid: !firrtl.uint<1>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> rows, annos;
  for (auto [i, n] : widgets) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("port", b.getStringAttr(n)), b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 5 && i == 7 ? 6 : i))}));
  if (bad != 6) named(c, oldTop)->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
  for (auto ref : {"hostReset", "ctrl_error_r_bits_id", "clockBridge_ctrl.r.bits.id", "clockBridge_ctrl.b.bits.id", "clockBridge_ctrl"})
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
  require(succeeded(goldengate::addControlReadArbiter(c, error)), error);
  require(succeeded(verify(*root)), "invalid read arbiter IR");
  auto helper = named(c, "GGControlReadArbiter"), top = named(c, newTop);
  require(helper.getNumPorts() == 93 && top.getNumPorts() == 44, "incorrect arbiter boundary");
  for (auto [i, n] : widgets) {
    auto ps = top.getPorts();
    auto p = llvm::find_if(ps, [&](const PortInfo &p) { return p.name.getValue() == n; });
    require(p != ps.end(), "missing B boundary");
    auto t = cast<BundleType>(p->type);
    require(t.getNumElements() == 1 && t.getElements()[0].name == "b", "consumed R field remains exposed");
  }
  Interpreter sim(helper); require(sim.regs.size() == 2, "incorrect lock state");
  std::mt19937_64 rng(182); unsigned index = 0, locked = 0, cases = 0;
  const std::pair<const char *, unsigned> leaves[]{{"resp", 2}, {"data", 32}, {"last", 1}, {"id", 12}, {"user", 1}};
  auto sample = [&](unsigned mask, bool ready, bool reset, bool last) {
    std::array<std::map<std::string, unsigned>, 12> payload;
    sim.memo = {{"reset", reset}, {"out_ready", ready}, {"lockIdx", index}, {"locked", locked}};
    unsigned chosen = index;
    if (!locked) for (unsigned off = 1; off <= 12; ++off) {
      unsigned i = (index + off) % 12;
      if (mask & (1u << i)) { chosen = i; break; }
    }
    for (unsigned i = 0; i < 12; ++i) {
      std::string p = "in_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = (mask >> i) & 1;
      for (auto [n, w] : leaves) {
        unsigned v = StringRef(n) == "last" ? unsigned(last) : unsigned(rng()) & ((1ULL << w) - 1);
        payload[i][n] = v; sim.memo[p + "bits_" + n] = v;
      }
    }
    bool valid = (mask >> chosen) & 1;
    require(sim.get("out_valid") == valid, "locked gaps or arbitration valid differs");
    for (unsigned i = 0; i < 12; ++i) require(sim.get("in_" + std::to_string(i) + "_ready") == bool(ready && chosen == i), "ready gated by valid or wrong source");
    for (auto [n, w] : leaves) require(sim.get(std::string("out_bits_") + n) == payload[chosen][n], "selected response payload differs");
    unsigned nextIndex = reset ? 0 : valid && ready && !locked ? chosen : index;
    unsigned nextLocked = reset ? 0 : valid && ready ? !last : locked;
    require((reset ? 0 : sim.eval(sim.drivers.at("lockIdx"))) == nextIndex, "round-robin index differs");
    require((reset ? 0 : sim.eval(sim.drivers.at("locked"))) == nextLocked, "burst lock/unlock differs");
    index = nextIndex; locked = nextLocked; ++cases;
  };
  // Exhaust every valid mask at every reachable RR cursor, including all-invalid
  // ready/payload behavior. Hold explicit states to isolate combinational choice.
  for (unsigned start = 0; start < 12; ++start) for (unsigned mask = 0; mask < 4096; ++mask) {
    index = start; locked = 0; sample(mask, mask & 1, false, mask & 2);
  }
  for (unsigned start = 0; start < 12; ++start) {
    index = start; locked = 0; sample(1u << start, true, false, false);
    require(locked && index == start, "first beat did not acquire burst lock");
    sample(4095 ^ (1u << start), true, false, true); require(locked, "gap switched away from locked source");
    sample(4095, false, false, true); require(locked, "backpressure unlocked burst");
    sample(4095, true, false, true); require(!locked, "accepted last did not unlock");
    sample(4095, true, true, false); require(!locked && !index, "reset did not override accepted nonfinal beat");
  }
  for (unsigned n = 0; n < 6000; ++n) sample(rng() % 4096, rng() & 1, n % 101 == 0, rng() & 1);
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  for (auto a : annos) {
    auto t = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = t.drop_front(t.find('>'));
    bool stays = i == 1 || i == 2 || i == 4;
    require(t == "~" + std::string(newTop) + "|" + (stays ? oldTop : newTop) + suffix.str(), "removed R or retained B target moved incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlReadArbiter(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  for (unsigned bad = 1; bad <= 9; ++bad) {
    auto r = fixture(ctx, bad); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlReadArbiter(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR");
  }
  llvm::outs() << "Control read arbiter: " << cases << " selection/burst/reset cases, targets and ten atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
