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
#include <vector>
#include "llvm/Support/MathExtras.h"
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
std::vector<std::pair<unsigned, std::string>> boundWidgets(unsigned count) {
  std::vector<std::pair<unsigned, std::string>> result;
  if (count == 11) for (auto [i, n] : widgets) result.emplace_back(i, n);
  else for (unsigned i = 0; i < count; i += 2)
    result.emplace_back(i, "widget_" + std::to_string(i) + "_ctrl");
  return result;
}
std::string regionName(unsigned count, unsigned i) {
  if (count != 11) return "Widget_" + std::to_string(i);
  const char *names[]{"BlockDevBridgeModule_0", "FASEDMemoryTimingModel_0", "TracerVBridgeModule_0",
      "TSIBridgeModule_0", "LoadMemWidget_0", "PeekPokeBridgeModule_0", "UARTBridgeModule_0",
      "ClockBridgeModule_0", "SimulationMaster_0", "ResetPulseBridgeModule_0", "CPUManagedStreamEngine_0"};
  return names[i];
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0, unsigned count = 11) {
  auto token = [](std::string s) { return "bundle<ready flip: uint<1>, valid: uint<1>, bits: " + s + ">"; };
  std::string control = "!firrtl.bundle<b flip: " + token("bundle<resp: uint<2>, id: uint<12>, user: uint<1>>") +
      ", r flip: " + token("bundle<resp: uint<2>, data: uint<32>, last: uint<1>, id: uint<12>, user: uint<1>>") + ">";
  std::string s = "module { firrtl.circuit \"" + std::string(oldTop) + "\" { firrtl.module @" + oldTop + "(";
  struct P { const char *name; const char *type; const char *dir; };
  const P ports[]{{"hostClock", "clock", "in"}, {"hostReset", "uint<1>", "in"},
      {"ctrl_error_r_ready", "uint<1>", "in"}, {"ctrl_error_r_valid", "uint<1>", "out"},
      {"ctrl_error_r_bits_last", "uint<1>", "out"}, {"ctrl_error_r_bits_id", "uint<12>", "out"},
      {"ctrl_error_r_bits_data", "uint<32>", "out"}, {"ctrl_error_r_bits_resp", "uint<2>", "out"},
      {"ctrl_error_r_bits_user", "uint<1>", "out"}};
  bool first = true;
  for (auto p : ports) {
    if (bad == 1 && StringRef(p.name) == "ctrl_error_r_bits_id") continue;
    if (!first) s += ", "; first = false;
    s += std::string(p.dir) + " %" + p.name + ": !firrtl." +
        (bad == 2 && StringRef(p.name) == "ctrl_error_r_bits_id" ? "uint<11>" : p.type);
  }
  for (auto [i, n] : boundWidgets(count)) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  for (unsigned i = 0; i < count; ++i) {
    auto bindings = boundWidgets(count);
    if (llvm::any_of(bindings, [&](const auto &w) { return w.first == i; })) continue;
    for (auto suffix : {"valid", "tag"}) {
      if (bad == 10 && i == 0 && StringRef(suffix) == "tag") continue;
      s += ", " + std::string(bad == 17 && i == 0 ? "out" : "in") + " %ctrl_read_tracker_deq_" +
          std::to_string(i) + "_" + suffix + ": !firrtl.uint<" +
          std::string(StringRef(suffix) == "tag" ? (bad == 18 && i == 0 ? "11" : "12") : "1") + ">";
    }
  }
  if (bad == 4) s += ", in %ctrl_read_arb_in_0_valid: !firrtl.uint<1>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> rows, annos;
  for (auto [i, n] : boundWidgets(count)) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 14 && i == 7 ? "WrongRegion" : regionName(count, i))),
      b.getNamedAttr("port", b.getStringAttr(n)), b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 5 && i == 7 ? 6 : bad == 15 && i == 7 ? count : bad == 16 && i == 7 ? -1 : int(i)))}));
  SmallVector<Attribute> catalog;
  for (unsigned i = 0; i < count; ++i) catalog.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 12 && i == 1 ? regionName(count, 0) : regionName(count, i))),
      b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 13 && i == 1 ? 0 : i))}));
  if (bad != 11) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto decoder = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGControlAddressDecode"),
        ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(catalog));
  }
  if (bad != 6) named(c, oldTop)->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
  const std::string targetPort = count == 11 ? "clockBridge_ctrl" : "widget_0_ctrl";
  for (const auto &ref : std::vector<std::string>{"hostReset", "ctrl_error_r_bits_id", targetPort + ".r.bits.id", targetPort + ".b.bits.id", targetPort})
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
void testCount(MLIRContext &ctx, unsigned count, unsigned &total) {
  auto root = fixture(ctx, 0, count);
  const unsigned sources = count + 1, width = llvm::Log2_64_Ceil(sources);
  const uint64_t fullMask = sources == 64 ? ~uint64_t(0) : (uint64_t(1) << sources) - 1;
  auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlReadArbiter(c, error)), error);
  require(succeeded(verify(*root)), "invalid read arbiter IR");
  auto helper = named(c, "GGControlReadArbiter"), top = named(c, newTop);
  auto widgetBindings = boundWidgets(count);
  require(helper.getNumPorts() == 9 + 7 * sources &&
      top.getNumPorts() == 9 + widgetBindings.size() + 7 * (count - widgetBindings.size()), "incorrect arbiter boundary");
  require(helper->getAttrOfType<IntegerAttr>("goldengate.readArbiterSources").getInt() == sources, "wrong source metadata");
  for (auto [i, n] : boundWidgets(count)) {
    auto ps = top.getPorts();
    auto p = llvm::find_if(ps, [&](const PortInfo &p) { return p.name.getValue() == n; });
    require(p != ps.end(), "missing B boundary");
    auto t = cast<BundleType>(p->type);
    require(t.getNumElements() == 1 && t.getElements()[0].name == "b", "consumed R field remains exposed");
  }
  // Check the real wrapper wiring for mapped, exposed and final error sources,
  // rather than only exercising an isolated arbitration helper.
  Interpreter wrapper(top);
  InstanceOp arb, inner;
  for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "readArbiter") arb = inst;
    if (inst.getName() == "sim") inner = inst;
  }
  auto instancePort = [&](InstanceOp inst, std::string name) -> Value {
    for (auto [i, n] : llvm::enumerate(inst.getPortNames()))
      if (cast<StringAttr>(n).getValue() == name) return inst.getResult(i);
    throw std::runtime_error("missing instance port " + name);
  };
  auto source = [&](Value value) { return wrapper.drivers.at(wrapper.key(value)); };
  auto fieldPath = [&](Value value, Value base, ArrayRef<StringRef> path) {
    for (auto name : llvm::reverse(path)) {
      auto subfield = value.getDefiningOp<SubfieldOp>();
      if (!subfield || subfield.getFieldName() != name) return false;
      value = subfield.getInput();
    }
    return value == base;
  };
  for (unsigned i = 0; i < sources; ++i) {
    std::string prefix = "in_" + std::to_string(i) + "_";
    auto ready = instancePort(arb, prefix + "ready"), valid = instancePort(arb, prefix + "valid");
    auto mapped = std::find_if(widgetBindings.begin(), widgetBindings.end(), [&](const auto &w) { return w.first == i; });
    if (i == count) {
      require(source(instancePort(inner, "ctrl_error_r_ready")) == ready, "error ready bound to wrong source");
      for (auto suffix : {"valid", "bits_id", "bits_resp", "bits_user", "bits_data", "bits_last"})
        require(source(instancePort(arb, prefix + suffix)) == instancePort(inner, std::string("ctrl_error_r_") + suffix), "error payload bound to wrong source");
    } else if (mapped != widgetBindings.end()) {
      auto base = instancePort(inner, mapped->second);
      require(fieldPath(source(valid), base, {"r", "valid"}), "widget valid bound to wrong source");
      for (auto leaf : {"id", "resp", "user", "data", "last"})
        require(fieldPath(source(instancePort(arb, prefix + "bits_" + leaf)), base, {"r", "bits", leaf}), "widget payload bound to wrong source");
      bool found = false;
      for (auto connect : top.getOps<StrictConnectOp>())
        if (fieldPath(connect.getDest(), base, {"r", "ready"})) { found = true; require(connect.getSrc() == ready, "widget ready bound to wrong source"); }
      require(found, "missing widget ready binding");
    } else {
      std::string retirement = "ctrl_read_tracker_deq_" + std::to_string(i) + "_";
      auto finalFire = source(instancePort(inner, retirement + "valid")).getDefiningOp<AndPrimOp>();
      require(finalFire && finalFire.getRhs() == instancePort(arb, prefix + "bits_last"), "retirement ignores its last beat");
      auto fire = finalFire.getLhs().getDefiningOp<AndPrimOp>();
      require(fire && fire.getLhs() == ready && fire.getRhs() == valid, "retirement does not fire on its own source");
      require(source(instancePort(inner, retirement + "tag")) == instancePort(arb, prefix + "bits_id"), "retirement tag crosses sources");
      for (auto suffix : {"valid", "bits_id", "bits_resp", "bits_user", "bits_data", "bits_last"})
        require(wrapper.key(source(instancePort(arb, prefix + suffix))) == "ctrl_read_arb_" + prefix + suffix, "unmapped source input not exposed");
      require(wrapper.drivers.at("ctrl_read_arb_" + prefix + "ready") == ready, "unmapped source ready not exposed");
    }
  }
  Interpreter sim(helper); require(sim.regs.size() == 2, "incorrect lock state");
  require(helper.getOps<RegOp>().empty(), "burst state must reset");
  for (auto r : helper.getOps<RegResetOp>()) {
    require(cast<UIntType>(r.getResult().getType()).getWidth().value() == (r.getName() == "lockIdx" ? width : 1), "incorrect lock state width");
    require(sim.key(r.getClockVal()) == "clock" && sim.key(r.getResetSignal()) == "reset" &&
        sim.eval(r.getResetValue()) == 0, "incorrect burst state clock/reset");
  }
  std::mt19937_64 rng(182 + count); unsigned index = 0, locked = 0, cases = 0;
  const std::pair<const char *, unsigned> leaves[]{{"resp", 2}, {"data", 32}, {"last", 1}, {"id", 12}, {"user", 1}};
  auto sample = [&](uint64_t mask, bool ready, bool reset, uint64_t lastMask) {
    std::vector<std::map<std::string, unsigned>> payload(sources);
    sim.memo = {{"reset", reset}, {"out_ready", ready}, {"lockIdx", index}, {"locked", locked}};
    unsigned chosen = index;
    if (!locked) for (unsigned off = 1; off <= sources; ++off) {
      unsigned i = (index + off) % sources;
      if (mask & (uint64_t(1) << i)) { chosen = i; break; }
    }
    for (unsigned i = 0; i < sources; ++i) {
      std::string p = "in_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = (mask >> i) & 1;
      for (auto [n, w] : leaves) {
        unsigned v = StringRef(n) == "last" ? unsigned((lastMask >> i) & 1) : unsigned(rng()) & ((1ULL << w) - 1);
        payload[i][n] = v; sim.memo[p + "bits_" + n] = v;
      }
    }
    bool valid = (mask >> chosen) & 1;
    require(sim.get("out_valid") == valid, "locked gaps or arbitration valid differs");
    for (unsigned i = 0; i < sources; ++i) require(sim.get("in_" + std::to_string(i) + "_ready") == bool(ready && chosen == i), "ready gated by valid or wrong source");
    for (auto [n, w] : leaves) require(sim.get(std::string("out_bits_") + n) == payload[chosen][n], "selected response payload differs");
    unsigned nextIndex = reset ? 0 : valid && ready && !locked ? chosen : index;
    unsigned nextLocked = reset ? 0 : valid && ready ? !payload[chosen]["last"] : locked;
    auto next = [&](std::string name) {
      auto r = sim.regs.at(name).getDefiningOp<RegResetOp>();
      return sim.eval(r.getResetSignal()) ? sim.eval(r.getResetValue()) : sim.eval(sim.drivers.at(name));
    };
    require(next("lockIdx") == nextIndex, "round-robin index differs");
    require(next("locked") == nextLocked, "burst lock/unlock differs");
    index = nextIndex; locked = nextLocked; ++cases;
  };
  // Exhaust U250 masks at every reachable cursor. Larger catalogs cover
  // each source in isolation and each locked gap, including source 63.
  for (unsigned start = 0; start < sources; ++start) {
    if (sources <= 12) {
      for (uint64_t mask = 0; mask <= fullMask; ++mask) {
        index = start; locked = 0; sample(mask, mask & 1, false, rng() & fullMask);
      }
    } else {
      for (bool lock : {false, true}) for (bool ready : {false, true}) {
        for (uint64_t mask : {uint64_t(0), fullMask}) {
          index = start; locked = lock; sample(mask, ready, false, rng() & fullMask);
        }
        for (unsigned i = 0; i < sources; ++i) {
          index = start; locked = lock; sample(uint64_t(1) << i, ready, false, rng() & fullMask);
        }
      }
    }
    index = start; locked = 0; sample(uint64_t(1) << start, true, false, 0);
    require(locked && index == start, "first beat did not acquire burst lock");
    sample(fullMask ^ (uint64_t(1) << start), true, false, fullMask); require(locked, "gap switched away from locked source");
    sample(fullMask, false, false, fullMask); require(locked, "backpressure unlocked burst");
    sample(fullMask, true, false, fullMask); require(!locked, "accepted last did not unlock");
    sample(fullMask, true, true, 0); require(!locked && !index, "reset did not override accepted nonfinal beat");
    for (bool lock : {false, true}) for (bool ready : {false, true}) {
      index = start; locked = lock; sample(0, ready, true, fullMask);
      require(!index && !locked, "reset with no valid response did not clear state");
    }
  }
  index = count; locked = 0;
  for (unsigned n = 0; n < 2 * sources; ++n) {
    sample(fullMask, true, false, fullMask);
    require(index == n % sources && !locked, "single-beat round-robin starvation");
  }
  for (unsigned n = 0; n < 6000; ++n)
    sample(rng() & fullMask, rng() & 1, n % 101 == 0, rng() & fullMask);
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  for (auto a : annos) {
    auto t = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = t.drop_front(t.find('>'));
    bool stays = i == 1 || i == 2 || i == 4;
    require(t == "~" + std::string(newTop) + "|" + (stays ? oldTop : newTop) + suffix.str(), "removed R or retained B target moved incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlReadArbiter(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  total += cases;
}
void test(MLIRContext &ctx) {
  unsigned total = 0, rejected = 7;
  for (unsigned count : {1u, 2u, 3u, 11u, 13u, 31u, 63u}) testCount(ctx, count, total);
  std::string error;
  for (unsigned bad = 1; bad <= 18; ++bad) {
    auto r = fixture(ctx, bad); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlReadArbiter(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR"); ++rejected;
  }
  for (unsigned count : {0u, 64u}) {
    auto r = fixture(ctx, 0, count); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlReadArbiter(ci, error)), "unsupported count accepted"); require(dump(*r) == s, "count rejection mutated IR"); ++rejected;
  }
  llvm::outs() << "Control read arbiter: " << total << " selection/burst/reset cases across 1,2,3,11,13,31,63 decoded regions; wrapper wiring, targets and " << rejected << " atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
