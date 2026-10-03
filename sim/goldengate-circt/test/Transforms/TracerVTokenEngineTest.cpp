// See LICENSE for license details.
#include "goldengate/TracerVTokenEngine.h"
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
const llvm::StringRef locals[]{"tiletrace_reset", "tiletrace_trace_retiredinsns_0_valid",
    "tiletrace_trace_retiredinsns_0_iaddr", "tiletrace_trace_retiredinsns_0_insn",
    "tiletrace_trace_retiredinsns_0_priv", "tiletrace_trace_retiredinsns_0_exception",
    "tiletrace_trace_retiredinsns_0_interrupt", "tiletrace_trace_retiredinsns_0_cause",
    "tiletrace_trace_retiredinsns_0_tval", "tiletrace_trace_time", "triggerCredit", "triggerDebit"};
const unsigned widths[]{1, 1, 40, 32, 3, 1, 1, 64, 40, 64, 1, 1};
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned retire = 1, unsigned latency = 1) {
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGPeekPokeBridgeControlWrapper" {
    firrtl.module @GGPeekPokeBridgeControlWrapper() {}
  } })", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  auto placeholder = *c.getOps<FModuleOp>().begin(); placeholder.erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
                            {b.getStringAttr("hostReset"), uint(1), Direction::In}};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 12; ++j) {
    auto type = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, uint(1)},
        {b.getStringAttr("valid"), false, uint(1)}, {b.getStringAttr("bits"), false, uint(widths[j])}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), type, j < 10 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("tracerv_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> attrs) { return b.getDictionaryAttr(attrs); };
  auto key = dict({str("class", "firechip.bridgeinterfaces.TraceBundleWidths"),
      b.getNamedAttr("retireWidth", b.getI64IntegerAttr(retire)), b.getNamedAttr("iaddrWidth", b.getI64IntegerAttr(40)),
      b.getNamedAttr("insnWidth", b.getI64IntegerAttr(32)), b.getNamedAttr("causeWidth", b.getI64IntegerAttr(64)),
      b.getNamedAttr("tvalWidth", b.getI64IntegerAttr(40))});
  SmallVector<Attribute> all{dict({str("class", "firesim.lib.bridgeutils.BridgeIOAnnotation"),
      str("widgetClass", "firechip.goldengateimplementations.TracerVBridgeModule"),
      str("target", "~FireSim|FireSim>tracerv"), b.getNamedAttr("widgetConstructorKey", key),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 12; ++j)
    all.push_back(dict({str("class", "midas.passes.fame.FAMEChannelConnectionAnnotation"),
        str("globalName", "tracerv_" + locals[j].str()),
        b.getNamedAttr("channelInfo", dict({str("class", "midas.passes.fame.PipeChannel"),
            b.getNamedAttr("latency", b.getI64IntegerAttr(latency))})),
        b.getNamedAttr(j < 10 ? "sources" : "sinks", b.getArrayAttr({b.getStringAttr(
            "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>token" + std::to_string(j) + ".bits")}))}));
  all.push_back(dict({str("class", "test.Annotation"), str("target", "~GGPeekPokeBridgeControlWrapper|GGPeekPokeBridgeControlWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(all)); return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>()) require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = APInt(width(r.getResult()), 0);
  }
  unsigned width(Value v) { return *cast<UIntType>(v.getType()).getWidth(); }
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  void put(Value v, llvm::StringRef path, uint64_t n) { memo[key(v) + (path.empty() ? "" : "." + path.str())] = APInt(64, n); }
  APInt output(Value v, llvm::StringRef path) { return eval(drivers.at(key(v) + "." + path.str())); }
  APInt eval(Value v) {
    auto k = key(v); unsigned w = width(v);
    if (memo.count(k)) return memo.at(k).zextOrTrunc(w);
    auto *op = v.getDefiningOp(); APInt n(w, 0);
    if (isa_and_nonnull<RegResetOp>(op)) n = state.lookup(v);
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
    else if (isa_and_nonnull<GEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).uge(eval(op->getOperand(1))));
    else if (isa_and_nonnull<LEQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)).ule(eval(op->getOperand(1))));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) + eval(op->getOperand(1)).zextOrTrunc(w);
    else if (isa_and_nonnull<CatPrimOp>(op)) n = eval(op->getOperand(0)).concat(eval(op->getOperand(1)));
    else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(w); memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()).isZero() ? eval(drivers.at(key(r.getResult()))) : eval(r.getResetValue());
    state = std::move(next);
  }
};
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVTokenEngine(c, error)), error);
  require(succeeded(verify(*root)), "engine/wrapper invalid IR");
  Interpreter sim(named(c, "GGTracerVTokenEngine"));
  std::map<std::string, Value> regs;
  for (auto r : sim.module.getOps<RegResetOp>()) regs[r.getName().str()] = r.getResult();
  require(regs.size() == 3 && sim.module.getOps<RegOp>().empty(), "wrong reset policy");
  auto read = [&](const std::string &n) { return sim.state.lookup(regs.at(n)).getZExtValue(); };
  std::mt19937_64 random(157); unsigned samples = 0;
  auto check = [&](uint64_t cycle, unsigned counter, unsigned prev, unsigned flags) {
    bool reset = flags & 1, input = flags & 2, output = flags & 4, ready = flags & 8,
         init = flags & 16, enable = flags & 32, trigger = flags & 64, targetReset = flags & 128, valid = flags & 256;
    uint64_t pc = random() & ((uint64_t(1) << 40) - 1);
    sim.state[regs.at("trace_cycle_counter")] = APInt(64, cycle);
    sim.state[regs.at("counter")] = APInt(1, counter); sim.state[regs.at("triggerReg")] = APInt(1, prev);
    sim.memo.clear(); sim.put(sim.arg(1), "", reset);
    sim.put(sim.arg(2), "toHost.hValid", input); sim.put(sim.arg(2), "fromHost.hReady", output);
    sim.put(sim.arg(2), "hBits.tiletrace_reset", targetReset);
    for (unsigned j = 1; j < 10; ++j) sim.put(sim.arg(2), "hBits." + locals[j].str(), j == 1 ? valid : j == 2 ? pc : random());
    sim.put(sim.arg(3), "initDone", init); sim.put(sim.arg(3), "traceEnable", enable); sim.put(sim.arg(3), "trigger", trigger);
    sim.put(sim.arg(4), "ready", ready);
    bool masked = valid && !targetReset, remain = !counter && masked;
    bool fire = input && output && ready && init;
    auto out = [&](unsigned i, llvm::StringRef p) { return sim.output(sim.arg(i), p).getZExtValue(); };
    require(out(2, "toHost.hReady") == (output && ready && init), "input readiness mismatch");
    require(out(2, "fromHost.hValid") == (input && ready && init), "output validity mismatch");
    require(out(2, "hBits.triggerCredit") == (trigger && !prev) && out(2, "hBits.triggerDebit") == (!trigger && prev), "credit history mismatch");
    require(out(4, "valid") == (remain && enable && input && output && init && trigger), "stream validity mismatch");
    APInt packet(512, cycle); packet |= APInt(512, pc).shl(64); packet |= APInt(512, masked).shl(127);
    require(sim.output(sim.arg(4), "bits") == packet && out(3, "tCycle") == cycle, "packet/cycle mismatch");
    sim.edge();
    require(read("trace_cycle_counter") == (reset ? 0 : cycle + uint64_t(fire)), "cycle next state mismatch");
    require(read("counter") == (reset || fire ? 0 : counter), "arm counter priority mismatch");
    require(read("triggerReg") == (reset ? 0 : fire ? trigger : prev), "trigger history next state mismatch");
    ++samples;
  };
  for (uint64_t cycle : {uint64_t(0), uint64_t(0xFFFFFFFF), UINT64_MAX})
    for (unsigned counter = 0; counter < 2; ++counter)
      for (unsigned prev = 0; prev < 2; ++prev)
        for (unsigned flags = 0; flags < 512; ++flags) check(cycle, counter, prev, flags);
  for (unsigned i = 0; i < 20000; ++i)
    check(read("trace_cycle_counter"), read("counter"), read("triggerReg"), unsigned(random()) & (i % 127 ? 510 : 511));
  llvm::outs() << samples << " native TracerV packet and next-state comparisons passed\n";
}
void wiring(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVTokenEngine(c, error)), error);
  auto wrapper = named(c, "GGTracerVTokenWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 5, "wrong wrapper boundary");
  InstanceOp inner, tracer;
  for (auto i : wrapper.getOps<InstanceOp>()) {
    if (i.getModuleName() == "GGTracerVTokenEngine") tracer = i; else inner = i;
  }
  require(inner && tracer, "missing bridge instance");
  for (unsigned mask = 0; mask < 1024; ++mask)
    for (unsigned ready = 0; ready < 4; ++ready) {
      sim.memo.clear();
      for (unsigned j = 0; j < 12; ++j) sim.put(inner.getResult(j + 2), j < 10 ? "valid" : "ready", j < 10 ? (mask >> j) & 1 : (ready >> (j - 10)) & 1);
      sim.put(tracer.getResult(2), "toHost.hReady", 1); sim.put(tracer.getResult(2), "fromHost.hValid", 1);
      require(sim.output(tracer.getResult(2), "toHost.hValid").getBoolValue() == (mask == 1023), "aggregate input valid mismatch");
      require(sim.output(tracer.getResult(2), "fromHost.hReady").getBoolValue() == (ready == 3), "aggregate output ready mismatch");
      for (unsigned j = 0; j < 12; ++j) {
        unsigned all = j < 10 ? 1023 : 3, own = 1 << (j < 10 ? j : j - 10), observed = j < 10 ? mask : ready;
        require(sim.output(inner.getResult(j + 2), j < 10 ? "ready" : "valid").getBoolValue() == ((observed | own) == all), "channel helper includes own predicate");
      }
    }
  // A stalled engine suppresses every endpoint even when other tokens arrive.
  sim.memo.clear();
  for (unsigned j = 0; j < 12; ++j) sim.put(inner.getResult(j + 2), j < 10 ? "valid" : "ready", 1);
  sim.put(tracer.getResult(2), "toHost.hReady", 0); sim.put(tracer.getResult(2), "fromHost.hValid", 0);
  for (unsigned j = 0; j < 12; ++j) require(sim.output(inner.getResult(j + 2), j < 10 ? "ready" : "valid").isZero(), "stall lost");
  llvm::outs() << "4096 all-channel helper comparisons passed\n";
}
std::string print(ModuleOp root) { std::string s; llvm::raw_string_ostream out(s); root.print(out); return s; }
void annotationsAndRejection(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::addTracerVTokenEngine(c, error)), error);
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(raw.size() == before.size(), "annotation loss");
  for (unsigned i = 0; i < raw.size(); ++i) require(cast<DictionaryAttr>(raw[i]).get("class") == cast<DictionaryAttr>(before[i]).get("class"), "annotation class changed");
  for (auto n : {"widgetConstructorKey", "channelMapping"}) require(cast<DictionaryAttr>(raw[0]).get(n) == cast<DictionaryAttr>(before[0]).get(n), "bridge identity changed");
  for (unsigned j = 0; j < 12; ++j) {
    auto a = cast<DictionaryAttr>(raw[j + 1]);
    for (unsigned end = 0; end < 2; ++end) {
      auto s = cast<StringAttr>(a.getAs<ArrayAttr>(end ? "sinks" : "sources")[0]);
      auto target = goldengate::resolveAnnotationTarget(c, s.getValue(), error);
      require(target && target->module.getModuleName() == (((j < 10) == bool(end)) ? "GGTracerVTokenEngine" : "GGPeekPokeBridgeControlWrapper"), "endpoint retarget mismatch");
    }
  }
  auto copied = goldengate::resolveAnnotationTarget(c, cast<DictionaryAttr>(raw[13]).getAs<StringAttr>("target").getValue(), error);
  require(copied && copied->module.getModuleName() == "GGTracerVTokenWrapper", "copied port not transferred");
  auto snapshot = print(*root);
  require(failed(goldengate::addTracerVTokenEngine(c, error)) && print(*root) == snapshot, "repeat pass mutated IR");
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto bad = fixture(ctx, mode == 0 ? 2 : 1, mode == 1 ? 0 : 1); auto bc = *bad->getOps<CircuitOp>().begin();
    OpBuilder b(&ctx);
    if (mode == 2) bc->removeAttr("rawAnnotations");
    if (mode >= 3) {
      auto old = bc->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> attrs(old.begin(), old.end());
      if (mode == 3) attrs.push_back(old[0]);
      else { NamedAttrList a(cast<DictionaryAttr>(old[2])); a.set("sources", cast<DictionaryAttr>(old[1]).get("sources")); attrs[2] = a.getDictionary(&ctx); }
      bc->setAttr("rawAnnotations", b.getArrayAttr(attrs));
    }
    auto saved = print(*bad);
    require(failed(goldengate::addTracerVTokenEngine(bc, error)) && !error.empty() && print(*bad) == saved, "unsupported boundary mutated IR");
  }
}
void triggerConfiguration(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTracerVTokenEngine(c, error)), error);
  auto before = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::addTracerVTriggerConfig(c, error)), error);
  require(succeeded(verify(*root)), "trigger stage invalid IR");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == before.size(), "trigger stage lost annotations");
  for (unsigned i = 0; i < raw.size(); ++i) require(cast<DictionaryAttr>(raw[i]).get("class") == cast<DictionaryAttr>(before[i]).get("class"), "trigger stage changed class");
  for (auto n : {"widgetConstructorKey", "channelMapping"}) require(cast<DictionaryAttr>(raw[0]).get(n) == cast<DictionaryAttr>(before[0]).get(n), "trigger stage changed constructor/mapping");
  Interpreter sim(named(c, "GGTracerVTriggerConfig"));
  const std::string names[]{"initDone", "traceEnable", "hostTriggerPCStartHigh", "hostTriggerPCStartLow",
      "hostTriggerPCEndHigh", "hostTriggerPCEndLow", "hostTriggerCycleCountStartHigh", "hostTriggerCycleCountStartLow",
      "hostTriggerCycleCountEndHigh", "hostTriggerCycleCountEndLow", "hostTriggerStartInst", "hostTriggerStartInstMask",
      "hostTriggerEndInst", "hostTriggerEndInstMask", "triggerSelector"};
  const unsigned widths[]{1, 1, 8, 32, 8, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32};
  std::map<std::string, Value> regs;
  for (auto r : sim.module.getOps<RegResetOp>()) regs[r.getName().str()] = r.getResult();
  require(regs.size() == 22 && sim.module.getOps<RegOp>().empty(), "trigger state/reset policy mismatch");
  auto read = [&](const std::string &n) { return sim.state.lookup(regs.at(n)).getZExtValue(); };
  auto set = [&](const std::string &n, uint64_t v) { sim.state[regs.at(n)] = APInt(sim.width(regs.at(n)), v); };
  std::mt19937_64 random(158); unsigned samples = 0;
  auto check = [&](uint64_t cycle, bool reset, bool valid, uint64_t pc, uint32_t insn, bool writes) {
    std::map<std::string, uint64_t> old, expected;
    for (auto [name, v] : regs) old[name] = sim.state.lookup(v).getZExtValue();
    expected = old; sim.memo.clear(); sim.put(sim.arg(1), "", reset);
    sim.put(sim.arg(2), "valid", valid); sim.put(sim.arg(2), "iaddr", pc); sim.put(sim.arg(2), "insn", insn);
    sim.put(sim.arg(3), "tCycle", cycle); sim.put(sim.arg(4), "wstrb", random() & 15);
    for (unsigned i = 0; i < 15; ++i) {
      bool write = writes && (random() & 1); uint32_t data = random();
      sim.put(sim.arg(4), "write[" + std::to_string(i) + "].valid", write);
      sim.put(sim.arg(4), "write[" + std::to_string(i) + "].bits", data);
      if (write) expected[names[i]] = uint64_t(data) & ((uint64_t(1) << widths[i]) - 1);
      require(sim.output(sim.arg(4), "read[" + std::to_string(i) + "].bits").getZExtValue() == (i < 2 ? old[names[i]] : 0), "decoded read data mismatch");
      require(sim.output(sim.arg(4), "read[" + std::to_string(i) + "].valid").getBoolValue() &&
              sim.output(sim.arg(4), "write[" + std::to_string(i) + "].ready").getBoolValue(), "decoded handshakes mismatch");
    }
    expected["triggerPCStart"] = (old[names[2]] << 32) | old[names[3]];
    expected["triggerPCEnd"] = (old[names[4]] << 32) | old[names[5]];
    expected["triggerCycleCountStart"] = (old[names[6]] << 32) | old[names[7]];
    expected["triggerCycleCountEnd"] = (old[names[8]] << 32) | old[names[9]];
    expected["triggerCycleCountVal"] = cycle >= old["triggerCycleCountStart"] && cycle <= old["triggerCycleCountEnd"];
    if (valid) {
      if (pc == old["triggerPCStart"]) expected["triggerPCValVec_0"] = 1;
      else if (pc == old["triggerPCEnd"]) expected["triggerPCValVec_0"] = 0;
      if (((old[names[10]] ^ insn) & old[names[11]]) == 0) expected["triggerInstValVec_0"] = 1;
      else if (((old[names[12]] ^ insn) & old[names[13]]) == 0) expected["triggerInstValVec_0"] = 0;
    }
    bool trigger = old["triggerSelector"] == 0 ? true : old["triggerSelector"] == 1 ? old["triggerCycleCountVal"] :
        old["triggerSelector"] == 2 ? old["triggerPCValVec_0"] : old["triggerSelector"] == 3 ? old["triggerInstValVec_0"] : false;
    require(sim.output(sim.arg(3), "trigger").getBoolValue() == trigger, "trigger selection mismatch");
    require(sim.output(sim.arg(3), "initDone").getZExtValue() == old["initDone"] &&
            sim.output(sim.arg(3), "traceEnable").getZExtValue() == old["traceEnable"], "control outputs mismatch");
    sim.edge();
    for (auto [name, v] : expected) require(read(name) == (reset ? uint64_t(name == "traceEnable") : v), "trigger next state mismatch: " + name);
    ++samples;
  };
  // Enumerate both history flags, cycle bounds, selector defaults and coincident
  // start/end matches. Writes may be simultaneous; limits use pre-edge words.
  for (unsigned selector : {0U, 1U, 2U, 3U, 4U, 0xFFFFFFFFU})
    for (unsigned flags = 0; flags < 64; ++flags)
      for (unsigned pattern = 0; pattern < 6; ++pattern) {
        for (auto [name, v] : regs) set(name, random());
        set("triggerSelector", selector); set("triggerPCValVec_0", (flags >> 2) & 1);
        set("triggerInstValVec_0", (flags >> 3) & 1); set("triggerCycleCountVal", (flags >> 4) & 1);
        set("triggerCycleCountStart", 4); set("triggerCycleCountEnd", pattern == 5 ? 3 : 8);
        uint64_t pc = random() & ((uint64_t(1) << 40) - 1); uint32_t insn = random();
        set("triggerPCStart", pattern & 1 ? pc : pc ^ 1); set("triggerPCEnd", pattern & 2 ? pc : pc ^ 2);
        set("hostTriggerStartInst", pattern & 1 ? insn : insn ^ 1); set("hostTriggerEndInst", pattern & 2 ? insn : insn ^ 2);
        set("hostTriggerStartInstMask", pattern == 4 ? 0 : UINT32_MAX); set("hostTriggerEndInstMask", UINT32_MAX);
        check(pattern == 0 ? 3 : pattern == 1 ? 4 : pattern == 2 ? 8 : pattern == 3 ? 9 : UINT64_MAX,
              flags & 1, flags & 2, pc, insn, flags & 32);
      }
  for (unsigned i = 0; i < 20000; ++i) {
    // Keep trigger selection and matching meaningful in a sustained host-clock
    // trajectory instead of allowing random 32-bit selectors to dominate.
    if (i % 31 == 0) set("triggerSelector", (i / 31) % 6);
    check(i, i % 127 == 0, random() & 1, random() & ((uint64_t(1) << 40) - 1), random(), i % 7 == 0);
  }
  auto inner = named(c, "GGTracerVTokenWrapper"); Interpreter tap(inner); InstanceOp tracer;
  for (auto i : inner.getOps<InstanceOp>()) if (i.getModuleName() == "GGTracerVTokenEngine") tracer = i;
  unsigned observedPort = inner.getNumPorts() - 1;
  require(inner.getPortName(observedPort) == "tracerv_trace", "missing trace observation port");
  for (unsigned flags = 0; flags < 4; ++flags) {
    tap.memo.clear(); tap.put(tracer.getResult(2), "hBits.tiletrace_reset", flags & 1);
    tap.put(tracer.getResult(2), "hBits.tiletrace_trace_retiredinsns_0_valid", (flags >> 1) & 1);
    tap.put(tracer.getResult(2), "hBits.tiletrace_trace_retiredinsns_0_iaddr", 0xABCDE12345);
    tap.put(tracer.getResult(2), "hBits.tiletrace_trace_retiredinsns_0_insn", 0x12345678);
    require(tap.output(tap.arg(observedPort), "valid").getBoolValue() == (flags == 2), "observation reset masking mismatch");
    require(tap.output(tap.arg(observedPort), "iaddr").getZExtValue() == 0xABCDE12345 &&
            tap.output(tap.arg(observedPort), "insn").getZExtValue() == 0x12345678, "observation payload mismatch");
  }
  auto outer = named(c, "GGTracerVTriggerWrapper");
  require(outer.getNumPorts() == 5 && outer.getPortName(4) == "tracerv_mcr", "wrong configuration wrapper boundary");
  auto metadata = sim.module->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(metadata.size() == 15, "wrong MMIO metadata count");
  for (unsigned i = 0; i < 15; ++i) {
    auto d = cast<DictionaryAttr>(metadata[i]);
    require(d.getAs<StringAttr>("name") == names[i] && d.getAs<IntegerAttr>("offset").getInt() == 4 * i &&
            d.getAs<BoolAttr>("readable").getValue() == (i < 2) && d.getAs<BoolAttr>("writeable").getValue(), "MMIO metadata mismatch");
  }
  auto copied = goldengate::resolveAnnotationTarget(c, cast<DictionaryAttr>(raw[13]).getAs<StringAttr>("target").getValue(), error);
  require(copied && copied->module.getModuleName() == "GGTracerVTriggerWrapper", "trigger copied-port target not transferred");
  auto snapshot = print(*root);
  require(failed(goldengate::addTracerVTriggerConfig(c, error)) && print(*root) == snapshot, "repeated trigger stage mutated IR");
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto bad = fixture(ctx); auto bc = *bad->getOps<CircuitOp>().begin();
    require(succeeded(goldengate::addTracerVTokenEngine(bc, error)), error); OpBuilder b(&ctx);
    if (mode == 0) bc->removeAttr("rawAnnotations");
    if (mode == 1) {
      auto e = named(bc, "GGTracerVTokenEngine"); NamedAttrList key(e->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor"));
      key.set("retireWidth", b.getI64IntegerAttr(2)); e->setAttr("goldengate.bridgeConstructor", key.getDictionary(&ctx));
    }
    if (mode == 2) {
      auto m = named(bc, "GGTracerVTokenWrapper"); m.insertPorts({{m.getNumPorts(), PortInfo(b.getStringAttr("tracerv_trace"), UIntType::get(&ctx, 1, false), Direction::Out)}});
    }
    auto saved = print(*bad);
    require(failed(goldengate::addTracerVTriggerConfig(bc, error)) && !error.empty() && print(*bad) == saved, "rejected trigger boundary mutated IR");
  }
  llvm::outs() << samples << " native trigger configuration comparisons and wrapper/identity checks passed\n";
}
} // namespace
int main() {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(ctx); wiring(ctx); annotationsAndRejection(ctx); triggerConfiguration(ctx);
    llvm::outs() << "TracerV identity, target transfer and atomic rejection passed\n"; return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
