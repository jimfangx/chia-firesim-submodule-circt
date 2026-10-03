// See LICENSE for license details.
#include "goldengate/BlockDevTokenEngine.h"
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGTSIBridgeBoundWrapper\" { firrtl.module @GGTSIBridgeBoundWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed");
  auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin());
  auto uint = [&](unsigned w) { return UIntType::get(&ctx, w, false); };
  auto bit = uint(1);
  const llvm::StringRef locals[]{"bdev_req_fwd", "bdev_data_fwd", "bdev_resp_rev", "reset",
      "bdev_req_rev", "bdev_data_rev", "bdev_resp_fwd", "bdev_info_nsectors", "bdev_info_max_req_len"};
  const std::vector<std::string> fields[]{
      {"bits_tag", "bits_len", "bits_offset", "bits_write", "valid"},
      {"bits_tag", "bits_data", "valid"}, {""}, {""}, {""}, {""},
      {"bits_tag", "bits_data", "valid"}, {""}, {""}};
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In}};
  NamedAttrList mapping;
  for (unsigned j = 0; j < 9; ++j) {
    FIRRTLBaseType payload = j > 6 ? FIRRTLBaseType(uint(32)) : FIRRTLBaseType(bit);
    if (j == 0 || j == 1 || j == 6) {
      SmallVector<BundleType::BundleElement> elements;
      for (auto &name : fields[j]) {
        unsigned w = name == "bits_data" ? 64 : name == "bits_len" || name == "bits_offset" ? 32 : 1;
        elements.push_back({b.getStringAttr(name), false, uint(w)});
      }
      payload = BundleType::get(&ctx, elements);
    }
    auto token = BundleType::get(&ctx, {{b.getStringAttr("ready"), true, bit},
        {b.getStringAttr("valid"), false, bit}, {b.getStringAttr("bits"), false, payload}});
    ports.push_back({b.getStringAttr("token" + std::to_string(j)), token, j < 4 ? Direction::Out : Direction::In});
    mapping.set(locals[j], b.getStringAttr("ep_0_" + locals[j].str()));
  }
  ports.push_back({b.getStringAttr("other"), uint(8), Direction::Out});
  b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto str = [&](llvm::StringRef n, llvm::StringRef v) { return b.getNamedAttr(n, b.getStringAttr(v)); };
  auto dict = [&](std::initializer_list<NamedAttribute> a) { return b.getDictionaryAttr(a); };
  auto path = [&](unsigned j, llvm::StringRef suffix) {
    return b.getStringAttr("~GGTSIBridgeBoundWrapper|GGTSIBridgeBoundWrapper>token" + std::to_string(j) + suffix.str());
  };
  SmallVector<Attribute> raw{dict({str("class", goldengate::AnnotationClasses::BridgeIO),
      str("widgetClass", "firechip.goldengateimplementations.BlockDevBridgeModule"), str("target", "~FireSim|FireSim>ep_0"),
      b.getNamedAttr("widgetConstructorKey", dict({str("class", "firechip.bridgeinterfaces.BlockDeviceConfig"), b.getNamedAttr("nTrackers", b.getI64IntegerAttr(1))})),
      b.getNamedAttr("channelMapping", mapping.getDictionary(&ctx))})};
  for (unsigned j = 0; j < 9; ++j) {
    bool forward = j == 0 || j == 1 || j == 6, pipe = j == 3 || j > 6;
    NamedAttrList info;
    info.set("class", b.getStringAttr(pipe ? goldengate::AnnotationClasses::PipeChannel : forward ? goldengate::AnnotationClasses::DecoupledForwardChannel : goldengate::AnnotationClasses::DecoupledReverseChannel));
    if (pipe) info.set("latency", b.getI64IntegerAttr(1));
    if (forward) {
      info.set(j < 4 ? "validSource" : "validSink", path(j, ".bits.valid"));
      info.set(j < 4 ? "readySink" : "readySource", path(j == 0 ? 4 : j == 1 ? 5 : 2, ".bits"));
    }
    SmallVector<Attribute> ends;
    for (auto &f : fields[j]) ends.push_back(path(j, f.empty() ? ".bits" : ".bits." + f));
    raw.push_back(dict({str("class", goldengate::AnnotationClasses::ChannelConnection), str("globalName", "ep_0_" + locals[j].str()),
        b.getNamedAttr("channelInfo", info.getDictionary(&ctx)), str("clock", "sameClock"),
        b.getNamedAttr(j < 4 ? "sources" : "sinks", b.getArrayAttr(ends))}));
  }
  raw.push_back(dict({str("class", "test.Annotation"), str("target", "~GGTSIBridgeBoundWrapper|GGTSIBridgeBoundWrapper>other")}));
  c->setAttr("rawAnnotations", b.getArrayAttr(raw)); return root;
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
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevTokenEngine(c, error)), error);
  require(succeeded(verify(*root)), "invalid BlockDev engine/wrapper");
  Interpreter sim(named(c, "GGBlockDevTokenEngine"));
  auto reg = *sim.module.getOps<RegResetOp>().begin();
  require(std::distance(sim.module.getOps<RegResetOp>().begin(), sim.module.getOps<RegResetOp>().end()) == 2 && std::distance(sim.module.getOps<RegOp>().begin(), sim.module.getOps<RegOp>().end()) == 1, "incorrect cycle/tracker state/reset");
  std::mt19937_64 rng(195); unsigned samples = 0, stalledReset = 0, resetOnFire = 0, reqWithoutOwnReady = 0, dataWithoutOwnReady = 0;
  auto check = [&](uint32_t cycle, unsigned flags) {
    auto flag = [&](unsigned n) { return bool(flags >> n & 1); };
    bool reset = flag(0), hostValid = flag(1), hostReady = flag(2), targetReset = flag(3);
    bool reqReady = flag(4), dataReady = flag(5), readValid = flag(6), writeValid = flag(7);
    bool readBusy = flag(8), returnWrite = flag(9), respReady = flag(10), reqValid = flag(11), dataValid = flag(12);
    bool readStallN = !readBusy || readValid, writeStallN = !returnWrite || writeValid;
    bool predicates[]{hostValid, hostReady, reqReady, dataReady, readStallN, writeStallN};
    auto except = [&](int own) { bool value = true; for (unsigned n = 0; n < 6; ++n) if (int(n) != own) value &= predicates[n]; return value; };
    bool fire = except(-1), queueReset = reset || (hostValid && hostReady && targetReset);
    uint64_t readData = rng(), targetData = rng(); uint32_t len = rng(), offset = rng(), sectors = rng(), maxLen = rng();
    bool readTag = rng() & 1, writeTag = rng() & 1, reqTag = rng() & 1, reqWrite = rng() & 1, dataTag = rng() & 1;
    sim.state[reg.getResult()] = APInt(24, cycle); sim.memo.clear();
    sim.put(sim.arg(1), "", reset);
    sim.put(sim.arg(2), "toHost.hValid", hostValid); sim.put(sim.arg(2), "fromHost.hReady", hostReady);
    sim.put(sim.arg(2), "hBits.reset", targetReset); sim.put(sim.arg(2), "hBits.bdev.resp.ready", respReady);
    sim.put(sim.arg(2), "hBits.bdev.req.valid", reqValid); sim.put(sim.arg(2), "hBits.bdev.data.valid", dataValid);
    sim.put(sim.arg(2), "hBits.bdev.req.bits.tag", reqTag); sim.put(sim.arg(2), "hBits.bdev.req.bits.len", len);
    sim.put(sim.arg(2), "hBits.bdev.req.bits.offset", offset); sim.put(sim.arg(2), "hBits.bdev.req.bits.write", reqWrite);
    sim.put(sim.arg(2), "hBits.bdev.data.bits.tag", dataTag); sim.put(sim.arg(2), "hBits.bdev.data.bits.data", targetData);
    sim.put(sim.arg(3), "ready", reqReady); sim.put(sim.arg(4), "ready", dataReady);
    sim.put(sim.arg(5), "valid", readValid); sim.put(sim.arg(5), "bits.tag", readTag); sim.put(sim.arg(5), "bits.data", readData);
    sim.put(sim.arg(6), "valid", writeValid); sim.put(sim.arg(6), "bits", writeTag);
    sim.put(sim.arg(7), "readRespBusy", readBusy); sim.put(sim.arg(7), "returnWrite", returnWrite);
    sim.put(sim.arg(8), "nsectors", sectors); sim.put(sim.arg(8), "max_req_len", maxLen);
    auto out = [&](unsigned i, llvm::StringRef n) { return sim.output(sim.arg(i), n).getZExtValue(); };
    require(out(9, "") == fire && out(2, "toHost.hReady") == fire && out(2, "fromHost.hValid") == fire, "BlockDev HostPort acceptance differs");
    require(out(13, "") == respReady, "scheduler response readiness differs");
    require(out(10, "") == queueReset, "queue reset incorrectly depends on queue/timing stalls");
    require(out(7, "wAckStallN") == writeStallN && out(7, "rRespStallN") == readStallN, "timing stall predicates differ");
    require(out(3, "valid") == (reqValid && except(2)) && out(4, "valid") == (dataValid && except(3)), "enqueues fail DecoupledHelper own-ready exclusion");
    require(out(5, "ready") == (except(4) && readBusy && respReady) && out(6, "ready") == (except(5) && returnWrite && respReady), "dequeues fail DecoupledHelper own-stall exclusion");
    require(out(2, "hBits.bdev.req.ready") == 1 && out(2, "hBits.bdev.data.ready") == 1, "target request/data readiness differs");
    require(out(2, "hBits.bdev.resp.valid") == (returnWrite || readBusy), "response valid incorrectly depends on functional queue valid");
    bool useRead = readValid && readBusy, useWrite = writeValid && returnWrite;
    require(out(2, "hBits.bdev.resp.bits.data") == (useRead ? readData : 0) && out(2, "hBits.bdev.resp.bits.tag") == (useRead ? readTag : useWrite ? writeTag : 0), "response priority/invalid payload zeroing differs");
    require(out(3, "bits.tag") == reqTag && out(3, "bits.len") == len && out(3, "bits.offset") == offset && out(3, "bits.write") == reqWrite && out(4, "bits.tag") == dataTag && out(4, "bits.data") == targetData, "request/data payload changed");
    require(out(2, "hBits.bdev.info.nsectors") == sectors && out(2, "hBits.bdev.info.max_req_len") == maxLen, "info payload changed");
    require(out(7, "tCycle") == cycle, "cycle observation differs");
    sim.edge(); uint32_t next = reset ? 0 : fire ? (cycle + 1) & 0xFFFFFFu : cycle;
    require(sim.state.lookup(reg.getResult()).getZExtValue() == next, "cycle host reset/increment/hold/overflow differs");
    ++samples; stalledReset += queueReset && !fire; resetOnFire += reset && fire;
    reqWithoutOwnReady += out(3, "valid") && !reqReady;
    dataWithoutOwnReady += out(4, "valid") && !dataReady;
  };
  for (uint32_t cycle : {0u, 1u, 0xFFFFFEu, 0xFFFFFFu})
    for (unsigned flags = 0; flags < 8192; ++flags) check(cycle, flags);
  for (unsigned n = 0; n < 10000; ++n) check(sim.state.lookup(reg.getResult()).getZExtValue(), rng() % 8192);
  require(stalledReset && resetOnFire && reqWithoutOwnReady && dataWithoutOwnReady, "missing reset/exclusion coverage");
  llvm::outs() << samples << " BlockDev token/reset/response/cycle comparisons passed\n";
}
// Independent arithmetic/state reference for Scala BlockDevBridgeModule's
// single tracker. In particular, completion uses OLD count and wins over a
// simultaneous allocation. The count has no reset and assertion reset gating
// does not suppress completion pulses or count updates.
void writeTracker(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevTokenEngine(c, error)), error);
  Interpreter sim(named(c, "GGBlockDevTokenEngine"));
  RegOp count;
  RegResetOp valid;
  for (auto r : sim.module.getOps<RegOp>()) { require(!count, "extra unreset tracker state"); count = r; }
  for (auto r : sim.module.getOps<RegResetOp>()) if (r.getName() == "wValid_0") valid = r;
  require(count && valid && count.getName() == "wBeatCounters_0", "missing write tracker state");
  require(sim.width(count.getResult()) == 32 && sim.width(valid.getResult()) == 1, "tracker width differs");
  SmallVector<AssertOp> assertions(sim.module.getOps<AssertOp>());
  require(assertions.size() == 3, "missing length/completion/data allocation assertions");
  struct Inputs {
    bool hostReset = false, targetReset = false, hostValid = true, hostReady = true;
    bool reqReady = true, dataReady = true, readBusy = false, readValid = false;
    bool returnWrite = false, writeValid = false;
    bool reqValid = false, reqWrite = false, reqTag = false, dataValid = false, dataTag = false;
    uint32_t len = 1;
  };
  unsigned samples = 0, completion = 0, simultaneous = 0, firstBeat = 0, resetCountChange = 0;
  unsigned stalledReset = 0, wrapped = 0, tagged = 0, assertionFailures[3]{};
  uint32_t expectedCount = 0;
  bool expectedValid = false;
  auto check = [&](const Inputs &in) {
    bool fire = in.hostValid && in.hostReady && in.reqReady && in.dataReady &&
        (!in.readBusy || in.readValid) && (!in.returnWrite || in.writeValid);
    bool reset = in.hostReset || (in.hostValid && in.hostReady && in.targetReset);
    bool writeRequest = in.reqValid && in.reqWrite && !in.reqTag;
    bool writeData = in.dataValid && !in.dataTag;
    bool done = writeData && expectedCount == 1;
    uint32_t nextCount = expectedCount;
    bool nextValid = expectedValid;
    if (fire) {
      if (done) nextValid = false;
      else if (writeRequest) {
        nextCount = uint32_t(uint64_t(in.len) * 64 - unsigned(writeData));
        nextValid = true;
      } else if (writeData) --nextCount;
    }
    if (reset) nextValid = false;
    sim.memo.clear();
    sim.put(sim.arg(1), "", in.hostReset);
    sim.put(sim.arg(2), "toHost.hValid", in.hostValid);
    sim.put(sim.arg(2), "fromHost.hReady", in.hostReady);
    sim.put(sim.arg(2), "hBits.reset", in.targetReset);
    sim.put(sim.arg(2), "hBits.bdev.req.valid", in.reqValid);
    sim.put(sim.arg(2), "hBits.bdev.req.bits.write", in.reqWrite);
    sim.put(sim.arg(2), "hBits.bdev.req.bits.tag", in.reqTag);
    sim.put(sim.arg(2), "hBits.bdev.req.bits.len", in.len);
    sim.put(sim.arg(2), "hBits.bdev.data.valid", in.dataValid);
    sim.put(sim.arg(2), "hBits.bdev.data.bits.tag", in.dataTag);
    sim.put(sim.arg(3), "ready", in.reqReady); sim.put(sim.arg(4), "ready", in.dataReady);
    sim.put(sim.arg(5), "valid", in.readValid); sim.put(sim.arg(6), "valid", in.writeValid);
    sim.put(sim.arg(7), "readRespBusy", in.readBusy); sim.put(sim.arg(7), "returnWrite", in.returnWrite);
    require(sim.output(sim.arg(11), "").getBoolValue() == (fire && done),
            "completion pulse differs under reset/stalls/invalid tracker");
    require(sim.output(sim.arg(12), "valid").getBoolValue() == (fire && in.reqValid && !in.reqWrite) &&
        sim.output(sim.arg(12), "bits").getZExtValue() == in.len, "committed read request/length differs");
    bool enables[]{fire && !reset, fire && done && !reset,
        fire && !done && !writeRequest && writeData && !reset};
    bool predicates[]{!in.reqValid || uint64_t(in.len) * 64 < 0xffffffffull,
        expectedValid, expectedValid};
    for (unsigned i = 0; i < 3; ++i) {
      require(sim.eval(assertions[i].getEnable()).getBoolValue() == enables[i], "write tracker assertion enable differs");
      require(sim.eval(assertions[i].getPredicate()).getBoolValue() == predicates[i], "write tracker assertion predicate differs");
      assertionFailures[i] += enables[i] && !predicates[i];
    }
    sim.edge();
    require(sim.state.lookup(count.getResult()).getZExtValue() == nextCount, "write tracker load/decrement/hold/priority differs");
    require(sim.state.lookup(valid.getResult()).getBoolValue() == nextValid, "write tracker valid/reset priority differs");
    ++samples; completion += fire && done; simultaneous += fire && done && writeRequest;
    firstBeat += fire && !done && writeRequest && writeData;
    resetCountChange += reset && expectedCount != nextCount;
    stalledReset += reset && !fire; wrapped += fire && !done && !writeRequest && writeData && expectedCount == 0;
    tagged += fire && ((in.reqValid && in.reqTag) || (in.dataValid && in.dataTag));
    expectedCount = nextCount; expectedValid = nextValid;
  };
  auto seed = [&](uint32_t beats, bool allocated) {
    expectedCount = beats; expectedValid = allocated;
    sim.state[count.getResult()] = APInt(32, beats); sim.state[valid.getResult()] = APInt(1, allocated);
  };
  // Cross arbitrary preexisting (including uninitialized/invalid) counter states
  // with allocation, tag, reset, and independent queue/channel stalls.
  for (uint32_t beats : {0u, 1u, 2u, 63u, 64u, 0xffffffffu})
    for (uint32_t len : {0u, 1u, 2u, 0x3ffffffu, 0x4000000u, 0x4000001u, 0x80000000u, 0xffffffffu})
      for (unsigned allocated = 0; allocated < 2; ++allocated)
        for (unsigned flags = 0; flags < 1024; ++flags) {
          Inputs in; in.len = len;
          in.reqValid = flags & 1; in.reqWrite = flags & 2; in.reqTag = flags & 4;
          in.dataValid = flags & 8; in.dataTag = flags & 16;
          in.hostReady = flags & 32; in.reqReady = flags & 64; in.dataReady = flags & 128;
          in.hostReset = flags & 256; in.targetReset = flags & 512;
          seed(beats, allocated); check(in);
        }
  // One complete request, including first beat on allocation, stalls while
  // collecting, and a new request on the final beat that completion discards.
  seed(0, false); Inputs in; in.reqValid = in.reqWrite = in.dataValid = true; check(in);
  in.reqValid = false;
  for (unsigned beat = 0; beat < 62; ++beat) { auto stalled = in; stalled.reqReady = false; check(stalled); check(in); }
  in.reqValid = true; check(in);
  require(expectedCount == 1 && !expectedValid, "final beat/new allocation priority trajectory differs");
  // Evolve independent state across host/timing stalls and resets. Assertions
  // are compared as observable predicates, including intentional violations.
  std::mt19937_64 rng(203);
  for (unsigned n = 0; n < 20000; ++n) {
    unsigned flags = rng(); Inputs random;
    random.hostReset = flags & 1; random.targetReset = flags & 2;
    random.hostValid = flags & 4; random.hostReady = flags & 8;
    random.reqReady = flags & 16; random.dataReady = flags & 32;
    random.readBusy = flags & 64; random.readValid = flags & 128;
    random.returnWrite = flags & 256; random.writeValid = flags & 512;
    random.reqValid = flags & 1024; random.reqWrite = flags & 2048; random.reqTag = flags & 4096;
    random.dataValid = flags & 8192; random.dataTag = flags & 16384;
    random.len = n & 1 ? uint32_t(rng()) : rng() % 8;
    check(random);
  }
  require(completion && simultaneous && firstBeat && resetCountChange && stalledReset && wrapped && tagged &&
      assertionFailures[0] && assertionFailures[1] && assertionFailures[2], "missing tracker priority/reset/arithmetic/assertion coverage");
  llvm::outs() << samples << " write tracker state/pulse/three-assertion comparisons passed; "
      << simultaneous << " final-beat allocation collisions, " << resetCountChange
      << " count updates during reset, failures " << assertionFailures[0] << "/"
      << assertionFailures[1] << "/" << assertionFailures[2] << "\n";
}
void wiring(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addBlockDevTokenEngine(c, error)), error);
  auto wrapper = named(c, "GGBlockDevTokenWrapper"); Interpreter sim(wrapper);
  require(wrapper.getNumPorts() == 14, "incorrect wrapper boundary");
  require(wrapper.getPortName(11) == "blockdev_write_latency_enq_valid", "missing completion boundary");
  InstanceOp inner, widget;
  for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getModuleName() == "GGBlockDevTokenEngine") widget = i; else inner = i; }
  require(inner && widget, "missing instances");
  sim.put(widget.getResult(11), "", 1);
  require(sim.output(sim.arg(11), "").getZExtValue() == 1, "completion pulse not copied to wrapper");
  sim.memo.clear(); sim.put(widget.getResult(11), "", 0);
  require(sim.output(sim.arg(11), "").getZExtValue() == 0, "completion pulse stuck high");
  require(wrapper.getPortName(12) == "blockdev_read_latency_enq", "missing read timing boundary");
  sim.put(widget.getResult(12), "valid", 1); sim.put(widget.getResult(12), "bits", 0xfedcba98u);
  require(sim.output(sim.arg(12), "valid").getBoolValue() &&
      sim.output(sim.arg(12), "bits").getZExtValue() == 0xfedcba98u, "read request/length not copied");
  require(wrapper.getPortName(13) == "blockdev_resp_ready", "missing response scheduler boundary");
  for (unsigned ready = 0; ready < 2; ++ready) {
    sim.memo.clear(); sim.put(widget.getResult(13), "", ready);
    require(sim.output(sim.arg(13), "").getZExtValue() == ready, "response readiness not copied");
  }
  unsigned samples = 0;
  for (unsigned valid = 0; valid < 16; ++valid) for (unsigned ready = 0; ready < 32; ++ready) for (unsigned gates = 0; gates < 4; ++gates) {
    sim.memo.clear();
    for (unsigned j = 0; j < 9; ++j) sim.put(inner.getResult(j + 2), j < 4 ? "valid" : "ready", j < 4 ? valid >> j & 1 : ready >> (j - 4) & 1);
    sim.put(widget.getResult(2), "toHost.hReady", gates & 1); sim.put(widget.getResult(2), "fromHost.hValid", gates >> 1);
    require(sim.output(widget.getResult(2), "toHost.hValid").getZExtValue() == (valid == 15) && sim.output(widget.getResult(2), "fromHost.hReady").getZExtValue() == (ready == 31), "aggregate handshake differs");
    for (unsigned j = 0; j < 9; ++j) {
      unsigned all = j < 4 ? 15 : 31, own = 1 << (j < 4 ? j : j - 4), observed = j < 4 ? valid : ready;
      bool gate = j < 4 ? gates & 1 : gates & 2;
      require(sim.output(inner.getResult(j + 2), j < 4 ? "ready" : "valid").getZExtValue() == (gate && (observed | own) == all), "nine-channel HostPort helper predicate differs");
    }
    ++samples;
  }
  sim.memo.clear();
  const std::vector<std::string> tokenFields[]{
      {"bits.bits_tag", "bits.bits_len", "bits.bits_offset", "bits.bits_write", "bits.valid"},
      {"bits.bits_tag", "bits.bits_data", "bits.valid"}, {"bits"}, {"bits"}, {"bits"}, {"bits"},
      {"bits.bits_tag", "bits.bits_data", "bits.valid"}, {"bits"}, {"bits"}};
  const std::vector<std::string> hostFields[]{
      {"bdev.req.bits.tag", "bdev.req.bits.len", "bdev.req.bits.offset", "bdev.req.bits.write", "bdev.req.valid"},
      {"bdev.data.bits.tag", "bdev.data.bits.data", "bdev.data.valid"}, {"bdev.resp.ready"}, {"reset"},
      {"bdev.req.ready"}, {"bdev.data.ready"}, {"bdev.resp.bits.tag", "bdev.resp.bits.data", "bdev.resp.valid"},
      {"bdev.info.nsectors"}, {"bdev.info.max_req_len"}};
  unsigned leaves = 0;
  for (unsigned j = 0; j < 9; ++j) for (unsigned k = 0; k < tokenFields[j].size(); ++k) {
    unsigned width = llvm::StringRef(hostFields[j][k]).ends_with(".data") ? 64 : hostFields[j][k].find(".len") != std::string::npos || hostFields[j][k].find(".offset") != std::string::npos || j > 6 ? 32 : 1;
    uint64_t value = width == 64 ? 0xF123456789ABCDEFull : width == 32 ? 0x87654321u : 1;
    if (j < 4) {
      sim.put(inner.getResult(j + 2), tokenFields[j][k], value);
      require(sim.output(widget.getResult(2), "hBits." + hostFields[j][k]).getZExtValue() == value, "toHost payload mapping differs");
    } else {
      sim.put(widget.getResult(2), "hBits." + hostFields[j][k], value);
      require(sim.output(inner.getResult(j + 2), tokenFields[j][k]).getZExtValue() == value, "fromHost payload mapping differs");
    }
    ++leaves;
  }
  llvm::outs() << samples << " nine-channel handshake masks and " << leaves << " payload leaves passed\n";
}
std::string print(ModuleOp root) { std::string s; llvm::raw_string_ostream o(s); root.print(o); return s; }
void annotationsAndRejection(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  auto old = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(succeeded(goldengate::addBlockDevTokenEngine(c, error)), error);
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(old.size() == raw.size(), "annotation loss");
  for (unsigned i = 0; i < raw.size(); ++i) require(cast<DictionaryAttr>(old[i]).get("class") == cast<DictionaryAttr>(raw[i]).get("class"), "annotation class changed");
  for (auto n : {"widgetConstructorKey", "channelMapping"}) require(cast<DictionaryAttr>(old[0]).get(n) == cast<DictionaryAttr>(raw[0]).get(n), "constructor/mapping changed");
  auto target = goldengate::resolveAnnotationTarget(c, cast<DictionaryAttr>(raw[0]).getAs<StringAttr>("target").getValue(), error);
  require(target && target->module.getModuleName() == "GGBlockDevTokenEngine" && *target->port == 2, "bridge identity not transferred");
  require(named(c, "GGBlockDevTokenEngine")->getAttr("goldengate.bridgeConstructor") == cast<DictionaryAttr>(old[0]).get("widgetConstructorKey"), "engine constructor changed");
  for (unsigned j = 0; j < 9; ++j) {
    require(cast<DictionaryAttr>(old[j + 1]).get("clock") == cast<DictionaryAttr>(raw[j + 1]).get("clock"), "channel clock changed");
    for (auto side : {"sources", "sinks"}) {
      auto ends = cast<DictionaryAttr>(raw[j + 1]).getAs<ArrayAttr>(side); require(bool(ends), "channel endpoints missing");
      for (auto e : ends) {
        auto t = goldengate::resolveAnnotationTarget(c, cast<StringAttr>(e).getValue(), error);
        require(t && t->module.getModuleName() == (((j < 4) == (llvm::StringRef(side) == "sources")) ? "GGTSIBridgeBoundWrapper" : "GGBlockDevTokenEngine"), "endpoint identity changed");
      }
    }
  }
  require(cast<DictionaryAttr>(raw[10]).getAs<StringAttr>("target") == "~GGBlockDevTokenWrapper|GGBlockDevTokenWrapper>other", "copied target not transferred");
  auto before = print(*root); require(failed(goldengate::addBlockDevTokenEngine(c, error)) && before == print(*root), "repeat mutated IR");
  for (unsigned mode = 0; mode < 16; ++mode) {
    auto bad = fixture(ctx); auto bc = *bad->getOps<CircuitOp>().begin(); OpBuilder b(&ctx);
    auto a = bc->getAttrOfType<ArrayAttr>("rawAnnotations"); SmallVector<Attribute> attrs(a.begin(), a.end());
    if (mode == 0) bc->removeAttr("rawAnnotations");
    else {
      if (mode == 1) attrs.push_back(a[0]);
      if (mode == 2) { NamedAttrList d(cast<DictionaryAttr>(a[0])); d.set("channelMapping", b.getDictionaryAttr({})); attrs[0] = d.getDictionary(&ctx); }
      if (mode == 3 || mode == 4) { NamedAttrList d(cast<DictionaryAttr>(a[0])); NamedAttrList key(cast<DictionaryAttr>(d.get("widgetConstructorKey"))); if (mode == 3) key.set("nTrackers", b.getI64IntegerAttr(2)); else key.set("class", b.getStringAttr("invalid")); d.set("widgetConstructorKey", key.getDictionary(&ctx)); attrs[0] = d.getDictionary(&ctx); }
      if (mode == 5) { NamedAttrList d(cast<DictionaryAttr>(a[2])); d.set("sources", cast<DictionaryAttr>(a[1]).get("sources")); attrs[2] = d.getDictionary(&ctx); }
      if (mode == 6) { NamedAttrList d(cast<DictionaryAttr>(a[4])); d.set("clock", b.getStringAttr("anotherClock")); attrs[4] = d.getDictionary(&ctx); }
      if (mode == 7) { NamedAttrList d(cast<DictionaryAttr>(a[4])); d.set("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)), b.getNamedAttr("latency", b.getI64IntegerAttr(0))})); attrs[4] = d.getDictionary(&ctx); }
      if (mode == 8 || mode == 9) { NamedAttrList d(cast<DictionaryAttr>(a[1])); NamedAttrList info(cast<DictionaryAttr>(d.get("channelInfo"))); info.set(mode == 8 ? "readySink" : "validSource", b.getStringAttr("~GGTSIBridgeBoundWrapper|GGTSIBridgeBoundWrapper>token2.bits")); d.set("channelInfo", info.getDictionary(&ctx)); attrs[1] = d.getDictionary(&ctx); }
      if (mode == 10) attrs.push_back(a[1]);
      if (mode == 11) { NamedAttrList d(cast<DictionaryAttr>(a[1])); d.set("sinks", cast<DictionaryAttr>(a[1]).get("sources")); attrs[1] = d.getDictionary(&ctx); }
      if (mode == 12) { NamedAttrList d(cast<DictionaryAttr>(a[1])); auto ends = d.get("sources"); SmallVector<Attribute> reversed(cast<ArrayAttr>(ends).begin(), cast<ArrayAttr>(ends).end()); std::swap(reversed[0], reversed[1]); d.set("sources", b.getArrayAttr(reversed)); attrs[1] = d.getDictionary(&ctx); }
      bc->setAttr("rawAnnotations", b.getArrayAttr(attrs));
      if (mode == 13) { auto top = named(bc, "GGTSIBridgeBoundWrapper"); b.setInsertionPointToEnd(bc.getBodyBlock()); auto user = b.create<FModuleOp>(bc.getLoc(), b.getStringAttr("User"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{}); b.setInsertionPointToStart(user.getBodyBlock()); b.create<InstanceOp>(bc.getLoc(), top, "top"); }
      if (mode == 14) { b.setInsertionPointToEnd(bc.getBodyBlock()); b.create<FModuleOp>(bc.getLoc(), b.getStringAttr("GGBlockDevTokenEngine"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{}); }
      if (mode == 15) bc.setName("UnexpectedTop");
    }
    auto before = print(*bad);
    require(failed(goldengate::addBlockDevTokenEngine(bc, error)) && !error.empty() && before == print(*bad), "malformed boundary accepted/mutated");
  }
  llvm::outs() << "annotation retention/targets and 16 atomic rejection cases passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); writeTracker(ctx); wiring(ctx); annotationsAndRejection(ctx); return 0; }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
