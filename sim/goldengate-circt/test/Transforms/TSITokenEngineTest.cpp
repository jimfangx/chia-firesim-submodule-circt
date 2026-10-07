// See LICENSE for license details.
#include "goldengate/TSITokenEngine.h"
#include "goldengate/AnnotationClasses.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, bool normalized = false) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGSimulationMasterBoundWrapper\" { firrtl.module @GGSimulationMasterBoundWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  auto placeholder = *c.getOps<FModuleOp>().begin(); placeholder.erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  auto bit = uint(1);
  auto forward = normalized
      ? BundleType::get(&ctx, {{b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, uint(32)}})
      : BundleType::get(&ctx, {{b.getStringAttr("bits"), false, uint(32)}, {b.getStringAttr("valid"), false, bit}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  const llvm::StringRef locals[]{"tsi_in_rev", "tsi_out_fwd", "reset", "tsi_in_fwd", "tsi_out_rev"};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 5; ++j) {
    auto token = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit}, {b.getStringAttr("valid"), false, bit},
        {b.getStringAttr("bits"), false, j == 1 || j == 3 ? FIRRTLBaseType(forward) : FIRRTLBaseType(bit)}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), token, j < 3 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("ep_3_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> a) { return b.getDictionaryAttr(a); };
  auto path = [&](unsigned j, llvm::StringRef suffix) {
    return b.getStringAttr("~GGSimulationMasterBoundWrapper|GGSimulationMasterBoundWrapper>token" + std::to_string(j) + suffix.str());
  };
  SmallVector<Attribute> raw{dict({str("class", goldengate::AnnotationClasses::BridgeIO),
      str("widgetClass", "firechip.goldengateimplementations.TSIBridgeModule"), str("target", "~FireSim|FireSim>ep_3"),
      b.getNamedAttr("widgetConstructorKey", dict({str("class", "firechip.bridgeinterfaces.TSIBridgeParams"), str("memoryRegionNameOpt", "MainMemory_0")})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 5; ++j) {
    NamedAttrList info;
    info.set("class", b.getStringAttr(j == 2 ? goldengate::AnnotationClasses::PipeChannel : j == 1 || j == 3 ? goldengate::AnnotationClasses::DecoupledForwardChannel : goldengate::AnnotationClasses::DecoupledReverseChannel));
    if (j == 2) info.set("latency", b.getI64IntegerAttr(1));
    if (j == 1 || j == 3) {
      info.set(j == 1 ? "validSource" : "validSink", path(j, ".bits.valid"));
      info.set(j == 1 ? "readySink" : "readySource", path(j == 1 ? 4 : 0, ".bits"));
    }
    SmallVector<Attribute> ends;
    if (j == 1 || j == 3) ends = {path(j, ".bits.bits"), path(j, ".bits.valid")}; else ends = {path(j, ".bits")};
    raw.push_back(dict({str("class", goldengate::AnnotationClasses::ChannelConnection), str("globalName", "ep_3_" + locals[j].str()),
        b.getNamedAttr("channelInfo", info.getDictionary(&ctx)), str("clock", "sameClock"),
        b.getNamedAttr(j < 3 ? "sources" : "sinks", b.getArrayAttr(ends))}));
  }
  raw.push_back(dict({str("class", "test.Annotation"), str("target", "~GGSimulationMasterBoundWrapper|GGSimulationMasterBoundWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(raw)); return root;
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
  APInt output(Value v, llvm::StringRef path) { return eval(drivers.at(key(v) + (path.empty() ? "" : "." + path.str()))); }
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
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)).zextOrTrunc(w) - eval(op->getOperand(1)).zextOrTrunc(w);
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
  require(succeeded(goldengate::addTSITokenEngine(c, error)), error);
  require(succeeded(verify(*root)), "invalid TSI engine/wrapper");
  Interpreter sim(named(c, "GGTSITokenEngine"));
  auto reg = *sim.module.getOps<RegResetOp>().begin();
  require(std::distance(sim.module.getOps<RegResetOp>().begin(), sim.module.getOps<RegResetOp>().end()) == 1 && sim.module.getOps<RegOp>().empty(), "incorrect scheduler state/reset");
  std::mt19937_64 rng(189); unsigned samples = 0, fires = 0, startsOnFire = 0, stalledReset = 0;
  auto check = [&](uint32_t count, unsigned flags, uint32_t step) {
    bool reset = flags & 1, valid = flags & 2, ready = flags & 4, targetReset = flags & 8, start = flags & 16;
    bool inReady = flags & 32, inValid = flags & 64, outReady = flags & 128, outValid = flags & 256;
    bool fire = valid && ready && count != 0;
    uint32_t inBits = rng(), outBits = rng();
    sim.state[reg.getResult()] = APInt(32, count); sim.memo.clear();
    sim.put(sim.arg(1), "", reset); sim.put(sim.arg(2), "toHost.hValid", valid); sim.put(sim.arg(2), "fromHost.hReady", ready);
    sim.put(sim.arg(2), "hBits.reset", targetReset); sim.put(sim.arg(2), "hBits.tsi_in_ready", inReady);
    sim.put(sim.arg(2), "hBits.tsi_out_valid", outValid); sim.put(sim.arg(2), "hBits.tsi_out_bits", outBits);
    sim.put(sim.arg(3), "step_size", step); sim.put(sim.arg(3), "start", start);
    sim.put(sim.arg(4), "valid", inValid); sim.put(sim.arg(4), "bits", inBits); sim.put(sim.arg(5), "ready", outReady);
    auto out = [&](unsigned i, llvm::StringRef n) { return sim.output(sim.arg(i), n).getZExtValue(); };
    require(out(2, "toHost.hReady") == fire && out(2, "fromHost.hValid") == fire, "HostPort acceptance differs");
    require(out(3, "done") == (count == 0), "raw done predicate differs");
    require(out(6, "") == (reset || fire && targetReset), "queue reset not qualified by accepted token");
    require(out(4, "ready") == (inReady && fire) && out(5, "valid") == (outValid && fire), "queue transfer gate differs");
    require(out(2, "hBits.tsi_in_valid") == inValid && out(2, "hBits.tsi_in_bits") == inBits && out(2, "hBits.tsi_out_ready") == outReady && out(5, "bits") == outBits, "payload/readiness changed by host stalls");
    sim.edge(); uint32_t next = reset ? 0 : start ? step : fire ? count - 1 : count;
    require(sim.state.lookup(reg.getResult()).getZExtValue() == next, "start/decrement/reset priority differs");
    ++samples; fires += fire; startsOnFire += start && fire; stalledReset += targetReset && !fire;
  };
  for (uint32_t count : {0u, 1u, 2u, 0xFFFFFFFFu})
    for (unsigned flags = 0; flags < 512; ++flags) check(count, flags, flags % 3 ? 0xFFFFFFFFu : 0u);
  for (unsigned i = 0; i < 20000; ++i) check(sim.state.lookup(reg.getResult()).getZExtValue(), rng() % 512, i % 3 ? rng() : 0);
  require(fires && startsOnFire && stalledReset, "missing gate/priority coverage");
  llvm::outs() << samples << " TSI cycle/reset/priority comparisons passed\n";
}
void wiring(MLIRContext &ctx, bool normalized = false) {
  auto root = fixture(ctx, normalized); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addTSITokenEngine(c, error)), error);
  auto wrapper = named(c, "GGTSITokenWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 7, "incorrect wrapper boundary");
  InstanceOp inner, tsi;
  for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getModuleName() == "GGTSITokenEngine") tsi = i; else inner = i; }
  require(inner && tsi, "missing instances");
  for (unsigned mask = 0; mask < 8; ++mask) for (unsigned ready = 0; ready < 4; ++ready) for (unsigned gates = 0; gates < 4; ++gates) {
    sim.memo.clear();
    for (unsigned j = 0; j < 5; ++j) sim.put(inner.getResult(j + 2), j < 3 ? "valid" : "ready", j < 3 ? mask >> j & 1 : ready >> (j - 3) & 1);
    sim.put(tsi.getResult(2), "toHost.hReady", gates & 1); sim.put(tsi.getResult(2), "fromHost.hValid", gates >> 1);
    require(sim.output(tsi.getResult(2), "toHost.hValid").getBoolValue() == (mask == 7) && sim.output(tsi.getResult(2), "fromHost.hReady").getBoolValue() == (ready == 3), "aggregate handshake differs");
    for (unsigned j = 0; j < 5; ++j) {
      unsigned all = j < 3 ? 7 : 3, own = 1 << (j < 3 ? j : j - 3), observed = j < 3 ? mask : ready;
      bool gate = j < 3 ? gates & 1 : gates & 2;
      require(sim.output(inner.getResult(j + 2), j < 3 ? "ready" : "valid").getBoolValue() == (gate && (observed | own) == all), "HostPort helper predicate differs");
    }
  }
  sim.memo.clear();
  sim.put(inner.getResult(2), "bits", 1); sim.put(inner.getResult(3), "bits.bits", 0xDEADBEEF); sim.put(inner.getResult(3), "bits.valid", 1); sim.put(inner.getResult(4), "bits", 1);
  sim.put(tsi.getResult(2), "hBits.tsi_in_bits", 0x12345678); sim.put(tsi.getResult(2), "hBits.tsi_in_valid", 1); sim.put(tsi.getResult(2), "hBits.tsi_out_ready", 1);
  require(sim.output(tsi.getResult(2), "hBits.tsi_in_ready").getBoolValue() && sim.output(tsi.getResult(2), "hBits.reset").getBoolValue() && sim.output(tsi.getResult(2), "hBits.tsi_out_bits").getZExtValue() == 0xDEADBEEF && sim.output(tsi.getResult(2), "hBits.tsi_out_valid").getBoolValue(), "toHost payload mapping differs");
  require(sim.output(inner.getResult(5), "bits.bits").getZExtValue() == 0x12345678 && sim.output(inner.getResult(5), "bits.valid").getBoolValue() && sim.output(inner.getResult(6), "bits").getBoolValue(), "fromHost payload mapping differs");
}
std::string print(ModuleOp root) { std::string s; llvm::raw_string_ostream o(s); root.print(o); return s; }
void annotationsAndRejection(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto old = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::addTSITokenEngine(c, error)), error);
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(old.size() == raw.size(), "annotation loss");
  for (unsigned i = 0; i < raw.size(); ++i) require(cast<DictionaryAttr>(old[i]).get("class") == cast<DictionaryAttr>(raw[i]).get("class"), "annotation class changed");
  for (auto n : {"widgetConstructorKey", "channelMapping"}) require(cast<DictionaryAttr>(old[0]).get(n) == cast<DictionaryAttr>(raw[0]).get(n), "constructor/mapping changed");
  for (unsigned j = 0; j < 5; ++j) for (auto side : {"sources", "sinks"})
    for (auto e : cast<DictionaryAttr>(raw[j + 1]).getAs<ArrayAttr>(side)) {
      auto t = goldengate::resolveAnnotationTarget(c, cast<StringAttr>(e).getValue(), error);
      require(t && t->module.getModuleName() == (((j < 3) == (llvm::StringRef(side) == "sources")) ? "GGSimulationMasterBoundWrapper" : "GGTSITokenEngine"), "endpoint identity changed");
    }
  require(cast<DictionaryAttr>(raw[6]).getAs<StringAttr>("target") == "~GGTSITokenWrapper|GGTSITokenWrapper>other", "copied target not transferred");
  auto before = print(*root); require(failed(goldengate::addTSITokenEngine(c, error)) && before == print(*root), "repeat mutated IR");
  for (unsigned mode = 0; mode < 9; ++mode) {
    auto bad = fixture(ctx); auto bc = *bad->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    auto a = bc->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> attrs(a.begin(), a.end());
    if (mode == 0) bc->removeAttr("rawAnnotations");
    else {
      if (mode == 1) attrs.push_back(a[0]);
      if (mode == 2) { NamedAttrList d(cast<DictionaryAttr>(a[0])); d.set("channelMapping", b.getDictionaryAttr({})); attrs[0] = d.getDictionary(&ctx); }
      if (mode == 3) { NamedAttrList d(cast<DictionaryAttr>(a[0])); d.set("widgetConstructorKey", b.getDictionaryAttr({})); attrs[0] = d.getDictionary(&ctx); }
      if (mode == 4) { NamedAttrList d(cast<DictionaryAttr>(a[2])); d.set("sources", cast<DictionaryAttr>(a[1]).get("sources")); attrs[2] = d.getDictionary(&ctx); }
      if (mode == 5) { NamedAttrList d(cast<DictionaryAttr>(a[3])); d.set("clock", b.getStringAttr("anotherClock")); attrs[3] = d.getDictionary(&ctx); }
      if (mode == 6) { NamedAttrList d(cast<DictionaryAttr>(a[3])); d.set("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)), b.getNamedAttr("latency", b.getI64IntegerAttr(0))})); attrs[3] = d.getDictionary(&ctx); }
      if (mode == 7) { NamedAttrList d(cast<DictionaryAttr>(a[2])); NamedAttrList info(cast<DictionaryAttr>(d.get("channelInfo"))); info.set("readySink", b.getStringAttr("~GGSimulationMasterBoundWrapper|GGSimulationMasterBoundWrapper>token0.bits")); d.set("channelInfo", info.getDictionary(&ctx)); attrs[2] = d.getDictionary(&ctx); }
      bc->setAttr("rawAnnotations", b.getArrayAttr(attrs));
      if (mode == 8) { auto top = named(bc, "GGSimulationMasterBoundWrapper"); b.setInsertionPointToEnd(bc.getBodyBlock()); auto user = b.create<FModuleOp>(bc.getLoc(), b.getStringAttr("User"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{}); b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(bc.getLoc(), top, "top"); }
    }
    auto before = print(*bad);
    require(failed(goldengate::addTSITokenEngine(bc, error)) && !error.empty() && before == print(*bad), "malformed boundary accepted/mutated");
  }
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); wiring(ctx); wiring(ctx, true); annotationsAndRejection(ctx); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
