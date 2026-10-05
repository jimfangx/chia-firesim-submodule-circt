// See LICENSE for license details.
#include "goldengate/ControlWriteArbiter.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <vector>
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
  for (auto [i, n] : boundWidgets(count)) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  if (bad == 4) s += ", in %ctrl_write_arb_in_0_valid: !firrtl.uint<1>";
  if (bad == 10) s += ", out %ctrl_write_tracker_deq_0_tag: !firrtl.uint<12>";
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
  for (const auto &ref : std::vector<std::string>{"hostReset", "ctrl_error_b_bits_id", targetPort + ".b.bits.id", targetPort})
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
void testCount(MLIRContext &ctx, unsigned count, unsigned &total) {
  auto root = fixture(ctx, 0, count);
  const unsigned sources = count + 1, width = llvm::Log2_64_Ceil(sources);
  const uint64_t fullMask = sources == 64 ? ~uint64_t(0) : (uint64_t(1) << sources) - 1;
  auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlWriteArbiter(c, error)), error);
  require(succeeded(verify(*root)), "invalid write arbiter IR");
  auto helper = named(c, "GGControlWriteArbiter"), top = named(c, newTop);
  auto widgetBindings = boundWidgets(count);
  require(helper.getNumPorts() == 7 + 5 * sources &&
      top.getNumPorts() == 8 + 5 * (count - widgetBindings.size()) + 2 * sources,
      "incorrect B arbiter boundary");
  require(helper->getAttrOfType<IntegerAttr>("goldengate.writeArbiterSources").getInt() == sources,
      "incorrect arbiter source metadata");
  require(helper.getOps<RegResetOp>().empty(), "RR lastGrant must remain uninitialized");
  auto rows = top->getAttrOfType<ArrayAttr>("goldengate.controlWriteResponseBindings");
  require(rows && rows.size() == widgetBindings.size(), "missing widget response bindings");
  for (auto a : rows) require(cast<DictionaryAttr>(a).getAs<StringAttr>("instance").getValue() == "sim", "binding does not locate inner widget");
  for (auto p : top.getPorts()) {
    require(!p.name.getValue().starts_with("ctrl_error_b_"), "error response remains outside");
    for (auto [i, n] : widgetBindings) require(p.name.getValue() != n, "consumed widget remains outside");
  }
  // Check the real wrapper wiring for mapped, exposed and final error sources,
  // rather than only exercising an isolated arbitration helper.
  Interpreter wrapper(top);
  InstanceOp arb, inner;
  for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "writeArbiter") arb = inst;
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
    std::string retirement = "ctrl_write_tracker_deq_" + std::to_string(i) + "_";
    auto ready = instancePort(arb, prefix + "ready"), valid = instancePort(arb, prefix + "valid");
    auto fire = wrapper.drivers.at(retirement + "valid").getDefiningOp<AndPrimOp>();
    require(fire && fire.getLhs() == ready && fire.getRhs() == valid, "retirement does not fire on its own source");
    require(wrapper.drivers.at(retirement + "tag") == instancePort(arb, prefix + "bits_id"), "retirement tag crosses sources");
    auto mapped = std::find_if(widgetBindings.begin(), widgetBindings.end(), [&](const auto &w) { return w.first == i; });
    if (i == count) {
      require(source(instancePort(inner, "ctrl_error_b_ready")) == ready, "error ready bound to wrong source");
      for (auto suffix : {"valid", "bits_id", "bits_resp", "bits_user"})
        require(source(instancePort(arb, prefix + suffix)) == instancePort(inner, std::string("ctrl_error_b_") + suffix), "error payload bound to wrong source");
    } else if (mapped != widgetBindings.end()) {
      auto base = instancePort(inner, mapped->second);
      require(fieldPath(source(valid), base, {"b", "valid"}), "widget valid bound to wrong source");
      for (auto leaf : {"id", "resp", "user"})
        require(fieldPath(source(instancePort(arb, prefix + "bits_" + leaf)), base, {"b", "bits", leaf}), "widget payload bound to wrong source");
      bool found = false;
      for (auto connect : top.getOps<StrictConnectOp>())
        if (fieldPath(connect.getDest(), base, {"b", "ready"})) { found = true; require(connect.getSrc() == ready, "widget ready bound to wrong source"); }
      require(found, "missing widget ready binding");
    } else {
      for (auto suffix : {"valid", "bits_id", "bits_resp", "bits_user"})
        require(wrapper.key(source(instancePort(arb, prefix + suffix))) == "ctrl_write_arb_" + prefix + suffix, "unmapped source input not exposed");
      require(wrapper.drivers.at("ctrl_write_arb_" + prefix + "ready") == ready, "unmapped source ready not exposed");
    }
  }
  Interpreter sim(helper); require(sim.regs.size() == 1 && sim.regs.count("lastGrant"), "incorrect RR state");
  require(cast<UIntType>(sim.regs.at("lastGrant").getType()).getWidth().value() == width, "incorrect RR state width");
  std::mt19937_64 rng(183 + count); unsigned index = 0, cases = 0;
  const std::pair<const char *, unsigned> leaves[]{{"resp", 2}, {"id", 12}, {"user", 1}};
  auto sample = [&](uint64_t mask, bool ready) {
    std::vector<std::map<std::string, unsigned>> payload(sources);
    sim.memo = {{"out_ready", ready}, {"lastGrant", index}};
    unsigned chosen = count;
    for (unsigned i = 0; i < sources; ++i) if (mask & (uint64_t(1) << i)) { chosen = i; break; }
    for (unsigned i = index + 1; i < sources; ++i) if (mask & (uint64_t(1) << i)) { chosen = i; break; }
    for (unsigned i = 0; i < sources; ++i) {
      std::string p = "in_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = (mask >> i) & 1;
      for (auto [n, w] : leaves) {
        unsigned v = unsigned(rng()) & ((1ULL << w) - 1);
        payload[i][n] = v; sim.memo[p + "bits_" + n] = v;
      }
    }
    bool valid = (mask >> chosen) & 1, maskedAny = false, earlierMasked = false, earlierValid = false;
    for (unsigned i = 0; i < sources; ++i) maskedAny |= i > index && ((mask >> i) & 1);
    require(sim.get("chosen") == chosen && sim.get("out_valid") == valid, "RR selection differs");
    unsigned accepted = 0;
    for (unsigned i = 0; i < sources; ++i) {
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
  // Exhaust the U250 validity/state space and the small-count geometries.
  // Larger counts cover every state and source, including the index-63 error
  // source; use a uint64 mask without ever shifting by 64.
  for (unsigned start = 0; start < (1u << width); ++start) {
    if (sources <= 12) {
      for (uint64_t mask = 0; mask <= fullMask; ++mask)
        for (bool ready : {false, true}) { index = start; sample(mask, ready); }
    } else {
      for (bool ready : {false, true}) {
        for (uint64_t mask : {uint64_t(0), fullMask}) { index = start; sample(mask, ready); }
        for (unsigned i = 0; i < sources; ++i) {
          for (uint64_t mask : {uint64_t(1) << i, fullMask ^ (uint64_t(1) << i)}) {
            index = start; sample(mask, ready);
          }
        }
      }
    }
  }
  index = count;
  for (unsigned n = 0; n < 2 * sources; ++n) { sample(fullMask, true); require(index == n % sources, "round-robin starvation"); }
  for (unsigned n = 0; n < 6000; ++n) sample(rng() & fullMask, rng() & 1);
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); unsigned i = 0;
  for (auto a : annos) {
    auto t = cast<DictionaryAttr>(a).getAs<StringAttr>("target").getValue(); auto suffix = t.drop_front(t.find('>'));
    bool stays = i != 0;
    require(t == "~" + std::string(newTop) + "|" + (stays ? oldTop : newTop) + suffix.str(), "consumed B or retained target moved incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlWriteArbiter(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  total += cases;
}
void test(MLIRContext &ctx) {
  unsigned total = 0, rejected = 0;
  for (unsigned count : {1u, 2u, 3u, 11u, 13u, 31u, 63u}) testCount(ctx, count, total);
  std::string error;
  for (unsigned bad = 1; bad <= 16; ++bad) {
    auto r = fixture(ctx, bad); auto ci = *r->getOps<CircuitOp>().begin(); auto s = dump(*r);
    require(failed(goldengate::addControlWriteArbiter(ci, error)), "malformed boundary accepted"); require(dump(*r) == s, "rejection mutated IR"); ++rejected;
  }
  for (unsigned count : {0u, 64u}) {
    auto r = fixture(ctx, 0, count); auto ci = *r->getOps<CircuitOp>().begin(); auto before = dump(*r);
    require(failed(goldengate::addControlWriteArbiter(ci, error)), "invalid catalog count accepted");
    require(dump(*r) == before, "invalid catalog count mutated IR"); ++rejected;
  }
  llvm::outs() << "Control write arbiter: " << total << " selection/acceptance/hold cases at 1,2,3,11,13,31,63 slaves, targets and "
               << rejected + 7 << " atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
