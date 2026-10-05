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
constexpr const char *oldTop = "GGControlReadDispatchWrapper", *newTop = "GGControlReadTrackerWrapper";
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0, unsigned count = 11,
                              bool noBindings = false) {
  auto bound = noBindings ? std::vector<std::pair<unsigned, std::string>>{} : boundWidgets(count);
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
        (bad == 2 && StringRef(p.name) == "ctrl_read_dispatch_track_tag" ? "uint<11>" :
         StringRef(p.name) == "ctrl_read_dispatch_track_target" ?
         "uint<" + std::to_string(bad == 10 ? 3 : llvm::Log2_64_Ceil(count + 1)) + ">" : p.type);
  }
  for (auto [i, n] : bound) s += ", " + std::string(bad == 3 && i == 7 ? "out" : "in") + " %" + n + ": " + control;
  if (bad == 4) s += ", in %ctrl_read_tracker_deq_0_valid: !firrtl.uint<1>";
  s += ") {} } }";
  auto root = parseSourceString<ModuleOp>(s, &ctx); require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&ctx); SmallVector<Attribute> rows, annos;
  for (auto [i, n] : bound) rows.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 14 && i == 7 ? "WrongRegion" : regionName(count, i))),
      b.getNamedAttr("port", b.getStringAttr(bad == 17 && i == 7 ? "uartBridge_ctrl" : n)),
      b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 5 && i == 7 ? 6 : bad == 15 && i == 7 ? count : bad == 16 && i == 7 ? -1 : int(i)))}));
  SmallVector<Attribute> catalog;
  for (unsigned i = 0; i < count; ++i) catalog.push_back(b.getDictionaryAttr({
      b.getNamedAttr("name", b.getStringAttr(bad == 12 && i == 1 ? regionName(count, 0) : bad == 18 && i == 1 ? "" : regionName(count, i))),
      b.getNamedAttr("slave", b.getI32IntegerAttr(bad == 13 && i == 1 ? 0 : i))}));
  if (bad != 11) {
    b.setInsertionPointToEnd(c.getBodyBlock());
    auto decoder = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGControlAddressDecode"),
        ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    decoder->setAttr("goldengate.controlRegions", b.getArrayAttr(catalog));
  }
  if (bad != 6) named(c, oldTop)->setAttr("goldengate.controlReadBindings", b.getArrayAttr(rows));
  const std::string targetPort = count == 11 ? "clockBridge_ctrl" : "widget_0_ctrl";
  std::vector<std::string> refs{"hostReset", "ctrl_read_dispatch_track_tag"};
  if (!bound.empty()) refs.push_back(targetPort + ".r.bits.id");
  for (const auto &ref : refs)
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
void testCount(MLIRContext &ctx, unsigned count, bool noBindings, unsigned &total) {
  const unsigned deqCount = count + 1, width = llvm::Log2_64_Ceil(deqCount), mask = (1U << width) - 1;
  auto bound = noBindings ? decltype(boundWidgets(count)){} : boundWidgets(count);
  auto root = fixture(ctx, 0, count, noBindings); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlReadTracker(c, error)), error);
  require(succeeded(verify(*root)), "invalid tracker IR");
  auto helper = named(c, "GGControlReadTracker"), top = named(c, newTop);
  require(helper.getNumPorts() == 6 + 4 * deqCount &&
      top.getNumPorts() == 6 + bound.size() + 2 * deqCount + 2 * (count - bound.size()), "incorrect tracker boundary");
  require(helper->getAttrOfType<IntegerAttr>("goldengate.trackerDequeuePorts").getInt() == deqCount &&
      helper->getAttrOfType<IntegerAttr>("goldengate.trackerRouteWidth").getInt() == width,
      "tracker geometry metadata differs");
  Interpreter wrapper(top);
  InstanceOp tracker, inner;
  for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "controlReadTracker") tracker = inst;
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
  require(wrapper.key(source(instancePort(tracker, "clock"))) == "hostClock" &&
      wrapper.key(source(instancePort(tracker, "reset"))) == "hostReset", "wrong tracker clock/reset");
  for (auto suffix : {"valid", "bits_tag", "bits_data"}) {
    const std::string dispatch = StringRef(suffix) == "valid" ? "valid" : StringRef(suffix) == "bits_tag" ? "tag" : "target";
    require(source(instancePort(tracker, std::string("enq_") + suffix)) ==
        instancePort(inner, "ctrl_read_dispatch_track_" + dispatch), "enqueue lost dispatch acceptance/tag/route");
  }
  require(source(instancePort(inner, "ctrl_read_dispatch_tracker_ready")) == instancePort(tracker, "enq_ready"),
      "dispatch lost outstanding queue capacity");
  for (unsigned i = 0; i < deqCount; ++i) {
    const std::string prefix = "deq_" + std::to_string(i) + "_";
    auto mapped = std::find_if(bound.begin(), bound.end(), [&](const auto &w) { return w.first == i; });
    if (i == count || mapped != bound.end()) {
      auto final = source(instancePort(tracker, prefix + "valid")).getDefiningOp<AndPrimOp>();
      require(bool(final), "missing final-beat retirement");
      auto fire = final.getLhs().getDefiningOp<AndPrimOp>();
      require(bool(fire), "missing response handshake");
      Value ready = fire.getLhs(), valid = fire.getRhs(), last = final.getRhs();
      if (i == count) {
        require(wrapper.key(ready) == "ctrl_error_r_ready" &&
            valid == instancePort(inner, "ctrl_error_r_valid") && last == instancePort(inner, "ctrl_error_r_bits_last") &&
            source(instancePort(tracker, prefix + "tag")) == instancePort(inner, "ctrl_error_r_bits_id"), "error retirement crosses sources");
      } else {
        auto base = instancePort(inner, mapped->second);
        Value outside;
        for (auto [j, p] : llvm::enumerate(top.getPorts())) if (p.name.getValue() == mapped->second) outside = top.getBodyBlock()->getArgument(j);
        require(fieldPath(ready, outside, {"r", "ready"}) && fieldPath(valid, base, {"r", "valid"}) &&
            fieldPath(last, base, {"r", "bits", "last"}) &&
            fieldPath(source(instancePort(tracker, prefix + "tag")), base, {"r", "bits", "id"}), "mapped retirement crosses sources");
      }
      for (unsigned bits = 0; bits < 8; ++bits) {
        wrapper.memo = {{wrapper.key(ready), bool(bits & 1)}, {wrapper.key(valid), bool(bits & 2)}, {wrapper.key(last), bool(bits & 4)}};
        require(wrapper.eval(final.getResult()) == (bits == 7), "stalled or nonfinal response retires a slot");
      }
    } else for (auto suffix : {"valid", "tag"})
      require(wrapper.key(source(instancePort(tracker, prefix + suffix))) == "ctrl_read_tracker_" + prefix + suffix,
          "unbound retirement input not exposed");
    for (auto suffix : {"data", "matches"}) {
      require(wrapper.drivers.at("ctrl_read_tracker_" + prefix + suffix) == instancePort(tracker, prefix + suffix),
          "tracker result bound to wrong source");
      unsigned expectedWidth = StringRef(suffix) == "data" ? width : 1;
      require(cast<UIntType>(instancePort(tracker, prefix + suffix).getType()).getWidth().value() == expectedWidth,
          "tracker output has wrong geometry");
    }
  }
  Interpreter sim(helper); require(sim.regs.size() == 192, "missing data/tag/free slots");
  SmallVector<AssertOp> assertions(helper.getOps<AssertOp>());
  require(assertions.size() == deqCount, "missing retirement assertions");
  for (auto [i, a] : llvm::enumerate(assertions)) {
    require(a.getClock() == helper.getBodyBlock()->getArgument(0) &&
        a.getEventControl() == EventControl::AtPosEdge && !a.getIsConcurrent(),
        "retirement assertion samples the wrong edge");
    require(a.getMessage() == "ar_queue " + std::to_string(i) +
        " tried to dequeue untracked transaction", "retirement diagnostic identity differs");
  }
  std::vector<unsigned> detected(deqCount), suppressed(deqCount), matched(deqCount);
  std::mt19937_64 rng(181); std::array<unsigned, 64> free, tags, data;
  for (unsigned i = 0; i < 64; ++i) { free[i] = 1; tags[i] = rng() % 64; data[i] = rng() & mask; }
  unsigned cases = 0;
  auto sample = [&](bool reset, bool enq, unsigned tag, unsigned payload,
                    const std::vector<unsigned> &deq, const std::vector<unsigned> &ids) {
    sim.memo = {{"reset", reset}, {"enq_valid", enq}, {"enq_bits_tag", tag}, {"enq_bits_data", payload}};
    for (unsigned i = 0; i < 64; ++i) {
      auto n = std::to_string(i); sim.memo["roq_free_" + n] = free[i]; sim.memo["roq_tags_" + n] = tags[i]; sim.memo["roq_data_" + n] = data[i];
    }
    for (unsigned i = 0; i < deqCount; ++i) {
      auto p = "deq_" + std::to_string(i) + "_"; sim.memo[p + "valid"] = deq[i]; sim.memo[p + "tag"] = ids[i];
    }
    bool ready = free[tag % 64]; require(sim.get("enq_ready") == ready, "tag alias capacity differs");
    for (unsigned i = 0; i < deqCount; ++i) {
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
    if (enq && ready) { unsigned slot = tag % 64; nf[slot] = 0; nt[slot] = tag / 64; nd[slot] = payload & mask; }
    for (unsigned i = 0; i < deqCount; ++i) if (deq[i]) nf[ids[i] % 64] = 1;
    if (reset) nf.fill(1);
    for (auto [n, v] : sim.regs) {
      uint64_t next = sim.eval(sim.drivers.at(n));
      if (auto r = v.getDefiningOp<RegResetOp>()) if (sim.eval(r.getResetSignal())) next = sim.eval(r.getResetValue());
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
  sample(true, true, 4095, 11, deq, ids);
  require(std::all_of(free.begin(), free.end(), [](unsigned x) { return x; }), "reset did not free every slot");
  sample(true, true, 4095, 5, deq, ids); // tag/data still write during reset
  for (unsigned n = 0; n < 1500; ++n) {
    for (unsigned i = 0; i < deqCount; ++i) { deq[i] = (rng() % 8) == 0; ids[i] = rng() % 4096; }
    sample(n % 97 == 0, rng() & 1, rng() % 4096, rng() & mask, deq, ids);
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
    require(cast<DictionaryAttr>(a).getAs<StringAttr>("class").getValue() == "test.Target", "annotation class changed");
    require(target == "~" + std::string(newTop) + "|" + (i == 1 ? oldTop : newTop) + suffix.str(), "consumed target transferred incorrectly"); ++i;
  }
  auto before = dump(*root); require(failed(goldengate::addControlReadTracker(c, error)), "repeat accepted"); require(dump(*root) == before, "repeat mutated IR");
  total += cases;
}
void test(MLIRContext &ctx) {
  unsigned total = 0, rejected = 0;
  for (unsigned count : {1U, 2U, 3U, 11U, 13U, 31U, 63U}) testCount(ctx, count, false, total);
  testCount(ctx, 3, true, total);
  std::string error;
  auto reject = [&](OwningOpRef<ModuleOp> r) {
    auto c = *r->getOps<CircuitOp>().begin(); auto before = dump(*r);
    require(failed(goldengate::addControlReadTracker(c, error)), "malformed boundary accepted");
    require(dump(*r) == before, "rejection mutated IR"); ++rejected;
  };
  for (unsigned bad = 1; bad <= 18; ++bad) reject(fixture(ctx, bad));
  reject(fixture(ctx, 0, 0)); reject(fixture(ctx, 0, 64));
  for (auto key : {"name", "slave"}) {
    auto r = fixture(ctx); auto c = *r->getOps<CircuitOp>().begin(); auto d = named(c, "GGControlAddressDecode");
    auto rows = d->getAttrOfType<ArrayAttr>("goldengate.controlRegions"); SmallVector<Attribute> out(rows.begin(), rows.end());
    NamedAttrList row(cast<DictionaryAttr>(out[1])); row.erase(key); out[1] = row.getDictionary(&ctx);
    d->setAttr("goldengate.controlRegions", ArrayAttr::get(&ctx, out)); reject(std::move(r));
  }
  llvm::outs() << "Control read tracker: " << total << " collision/retirement/reset cycles across 1,2,3,11,13,31,63 regions and empty bindings; wrapper final-beat wiring, targets and " << rejected + 8 << " atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
