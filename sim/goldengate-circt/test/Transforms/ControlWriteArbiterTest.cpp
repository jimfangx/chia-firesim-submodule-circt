// See LICENSE for license details.
#include "goldengate/ControlWriteArbiter.h"
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
constexpr const char *oldTop = "GGControlReadArbiterWrapper", *newTop = "GGControlWriteArbiterWrapper";
const std::pair<unsigned, const char *> widgets[]{{2, "tracerv_ctrl"}, {4, "loadmem_ctrl"},
    {5, "peekPokeBridge_ctrl"}, {6, "uartBridge_ctrl"}, {7, "clockBridge_ctrl"},
    {9, "resetBridge_ctrl"}, {10, "cpuStream_ctrl"}};
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  auto token = [](std::string s) { return "bundle<ready flip: uint<1>, valid: uint<1>, bits: " + s + ">"; };
  std::string control = "!firrtl.bundle<b flip: " + token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>") + ">";
  std::string s = "module { firrtl.circuit \"" + std::string(oldTop) + "\" { firrtl.module @" + oldTop + "(";
  struct P { const char *name; const char *type; const char *dir; };
  const P ports[]{{"hostClock", "clock", "in"}, {"hostReset", "uint<1>", "in"},
      {"ctrl_error_b_ready", "uint<1>", "in"}, {"ctrl_error_b_valid", "uint<1>", "out"},
      {"ctrl_error_b_bits_id", "uint<12>", "out"}, {"ctrl_error_b_bits_resp", "uint<2>", "out"},
      {"ctrl_error_b_bits_user", "uint<1>", "out"}};
  bool first = true;
  for (auto p : ports) {
    if (bad == 1 && StringRef(p.name) == "ctrl_error_b_bits_id") continue;
    if (!first) s += ", "; first = false;
    s += std::string(p.dir) + " %" + p.name + ": !firrtl." +
        (bad == 2 && StringRef(p.name) == "ctrl_error_b_bits_id" ? "uint<11>" : p.type);
  }
  for (auto [i, n] : widgets) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  if (bad == 4) s += ", in %ctrl_write_arb_in_0_valid: !firrtl.uint<1>";
  if (bad == 10) s += ", out %ctrl_write_tracker_deq_0_tag: !firrtl.uint<12>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> rows, annos;
  for (auto [i, n] : widgets) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("port", b.getStringAttr(n)), b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 5 && i == 7 ? 6 : i))}));
  if (bad != 6) named(c, oldTop)->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
  for (auto ref : {"hostReset", "ctrl_error_b_bits_id", "clockBridge_ctrl.b.bits.id", "clockBridge_ctrl"})
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
    else if (isa_and_nonnull<GTPrimOp>(op)) n = eval(op->getOperand(0)) > eval(op->getOperand(1));
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
  require(succeeded(goldengate::addControlWriteArbiter(c, error)), error);
  require(succeeded(verify(*root)), "invalid write arbiter IR");
  auto helper = named(c, "GGControlWriteArbiter"), top = named(c, newTop);
  require(helper.getNumPorts() == 67 && top.getNumPorts() == 52, "incorrect B arbiter boundary");
  require(helper.getOps<RegResetOp>().empty(), "RR lastGrant must remain uninitialized");
  auto rows = top->getAttrOfType<ArrayAttr>("goldengate.controlWriteResponseBindings");
  require(rows && rows.size() == 7, "missing widget response bindings");
  for (auto a : rows) require(cast<DictionaryAttr>(a).getAs<StringAttr>("instance").getValue() == "sim", "binding does not locate inner widget");
  for (auto p : top.getPorts()) {
    require(!p.name.getValue().starts_with("ctrl_error_b_"), "error response remains outside");
    for (auto [i, n] : widgets) require(p.name.getValue() != n, "consumed widget remains outside");
  }
  Interpreter sim(helper); require(sim.regs.size() == 1 && sim.regs.count("lastGrant"), "incorrect RR state");
  std::mt19937_64 rng(183); unsigned index = 0, cases = 0;
  const std::pair<const char *, unsigned> leaves[]{{"resp", 2}, {"id", 12}, {"user", 1}};
  auto sample = [&](unsigned mask, bool ready) {
    std::array<std::map<std::string, unsigned>, 12> payload;
    sim.memo = {{"out_ready", ready}, {"lastGrant", index}};
    unsigned chosen = 11;
    for (unsigned i = 0; i < 12; ++i) if (mask & (1u << i)) { chosen = i; break; }
    for (unsigned i = index + 1; i < 12; ++i) if (mask & (1u << i)) { chosen = i; break; }
    for (unsigned i = 0; i < 12; ++i) {
      std::string p = "in_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = (mask >> i) & 1;
      for (auto [n, w] : leaves) {
        unsigned v = unsigned(rng()) & ((1ULL << w) - 1);
        payload[i][n] = v; sim.memo[p + "bits_" + n] = v;
      }
    }
    bool valid = (mask >> chosen) & 1, maskedAny = false, earlierMasked = false, earlierValid = false;
    for (unsigned i = 0; i < 12; ++i) maskedAny |= i > index && ((mask >> i) & 1);
    require(sim.get("chosen") == chosen && sim.get("out_valid") == valid, "RR selection differs");
    unsigned accepted = 0;
    for (unsigned i = 0; i < 12; ++i) {
      bool grant = (!earlierMasked && i > index) || (!maskedAny && !earlierValid);
      bool inReady = sim.get("in_" + std::to_string(i) + "_ready");
      require(inReady == (ready && grant), "prefix ready differs, including invalid sources");
      accepted += inReady && ((mask >> i) & 1);
      earlierMasked |= i > index && ((mask >> i) & 1); earlierValid |= (mask >> i) & 1;
    }
    require(accepted == unsigned(ready && valid), "accepted B response count differs");
    for (auto [n, w] : leaves) require(sim.get(std::string("out_bits_") + n) == payload[chosen][n], "selected response payload differs");
    unsigned next = valid && ready ? chosen : index;
    require(sim.eval(sim.drivers.at("lastGrant")) == next, "RR hold or advance differs");
    index = next; ++cases;
  };
  // Four-bit uninitialized state can hold all sixteen two-state values.
  for (unsigned start = 0; start < 16; ++start) for (unsigned mask = 0; mask < 4096; ++mask)
    for (bool ready : {false, true}) { index = start; sample(mask, ready); }
  index = 11;
  for (unsigned n = 0; n < 24; ++n) { sample(4095, true); require(index == n % 12, "round-robin starvation"); }
  for (unsigned n = 0; n < 6000; ++n) sample(rng() % 4096, rng() & 1);
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  for (auto a : annos) {
    auto t = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = t.drop_front(t.find('>'));
    bool stays = i != 0;
    require(t == "~" + std::string(newTop) + "|" + (stays ? oldTop : newTop) + suffix.str(), "consumed B or retained target moved incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlWriteArbiter(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  for (unsigned bad = 1; bad <= 10; ++bad) {
    auto r = fixture(ctx, bad); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlWriteArbiter(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR");
  }
  llvm::outs() << "Control write arbiter: " << cases << " selection/acceptance/hold cases, targets and eleven atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
