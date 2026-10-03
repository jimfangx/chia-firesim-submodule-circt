// See LICENSE for license details.
#include "goldengate/FASEDIngressAWQueue.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <deque>
#include <map>
#include <functional>
#include <random>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, llvm::StringRef message) {
  if (!ok) throw std::runtime_error(message.str());
}
FModuleOp named(CircuitOp c, llvm::StringRef name) {
  for (auto m : c.getOps<FModuleOp>()) if (m.getName() == name) return m;
  throw std::runtime_error("missing module");
}
const llvm::StringRef fields[]{"user","id","region","qos","prot","cache","lock","burst","size","len","addr"};
const unsigned widths[]{1,4,4,4,3,4,1,2,3,8,35};
struct Request {
  std::array<uint64_t, 11> values;
  APInt pack() const {
    APInt value(69, 0);
    for (unsigned i = 0; i < 11; ++i) value = (value << widths[i]) | APInt(69, values[i]);
    return value;
  }
};
// Interpret the generated FIRRTL, including the real RAM ports. The deque
// reference tracks request ordering independently of the hardware pointers.
struct Interpreter {
  FModuleOp module;
  MemOp ram;
  std::array<APInt, 10> memory;
  std::map<std::string, Value> drivers;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  unsigned width(Value v) { return cast<UIntType>(v.getType()).getWidthOrSentinel(); }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    require(std::distance(m.getOps<MemOp>().begin(), m.getOps<MemOp>().end()) == 1, "queue needs one memory");
    ram = *m.getOps<MemOp>().begin();
    require(ram.getDepth() == 10 && ram.getDataType() == UIntType::get(m.getContext(), 69, false) &&
        ram.getReadLatency() == 0 && ram.getWriteLatency() == 1 && ram.getRuw() == RUWAttr::Undefined &&
        ram.getNumResults() == 2 && ram.getPortKind(size_t(0)) == MemOp::PortKind::Read &&
        ram.getPortKind(size_t(1)) == MemOp::PortKind::Write, "wrong memory geometry/latency/ports");
    for (unsigned i = 0; i < memory.size(); ++i) memory[i] = (APInt(69, i) << 64) | APInt(69, 0xAC000000u + i);
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  APInt drive(Value root, llvm::StringRef field) { return eval(drivers.at(key(root) + "." + field.str())); }
  APInt eval(Value v) {
    auto k = key(v);
    if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); APInt n(width(v), 0);
    if (isa_and_nonnull<RegResetOp>(op)) {
      if (state.count(v)) n = state.lookup(v);
    } else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = ~eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = APInt(1, eval(op->getOperand(0)) == eval(op->getOperand(1)));
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)).zext(width(v)) + eval(op->getOperand(1)).zext(width(v));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(!eval(op->getOperand(0)).isZero() ? 1 : 2));
    else if (auto cat = dyn_cast_or_null<CatPrimOp>(op)) {
      auto a = eval(op->getOperand(0)), b = eval(op->getOperand(1));
      n = (a.zext(width(v)) << b.getBitWidth()) | b.zext(width(v));
    } else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = eval(bits.getInput()).lshr(bits.getLo()).trunc(width(v));
    else if (auto f = dyn_cast_or_null<SubfieldOp>(op)) {
      require(f.getInput() == ram.getResult(0) && f.getFieldName() == "data", "unsupported memory access");
      require(drive(ram.getResult(0), "en").getZExtValue() == 1, "queue read disabled");
      n = memory.at(drive(ram.getResult(0), "addr").getZExtValue());
    } else throw std::runtime_error("unsupported operation or missing driver");
    n = n.zextOrTrunc(width(v)); memo.insert_or_assign(k, n); return n;
  }
  void edge() {
    llvm::DenseMap<Value, APInt> next;
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = !eval(r.getResetSignal()).isZero() ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    Value writer = ram.getResult(1);
    if (!drive(writer, "en").isZero() && !drive(writer, "mask").isZero())
      memory.at(drive(writer, "addr").getZExtValue()) = drive(writer, "data");
    state = std::move(next);
  }
};
OwningOpRef<ModuleOp> fixture(MLIRContext &context, unsigned variant = 0) {
  std::string text = R"(module {
    firrtl.circuit "GGFASEDHostOutstandingWrapper" {
      firrtl.module @GGFASEDHostOutstandingWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        out %fased_ingress: !firrtl.bundle<hReady flip: uint<1>, hValid: uint<1>, hBits: bundle<
          aw: bundle<valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, region: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>>>,
          w: bundle<valid: uint<1>, bits: bundle<user: uint<1>, strb: uint<8>, id: uint<4>, last: uint<1>, data: uint<64>>>,
          ar: bundle<valid: uint<1>, bits: bundle<user: uint<1>, id: uint<4>, region: uint<4>, qos: uint<4>, prot: uint<3>, cache: uint<4>, lock: uint<1>, burst: uint<2>, size: uint<3>, len: uint<8>, addr: uint<35>>>>>,
        out %fased_ingress_reset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
    } })";
  auto replace = [&](llvm::StringRef a, llvm::StringRef b) { auto p = text.find(a.str()); require(p != std::string::npos, "fixture replacement missing"); text.replace(p, a.size(), b.str()); };
  if (variant == 1) replace("out %fased_ingress:", "in %fased_ingress:");
  if (variant == 2) replace("addr: uint<35>", "addr: uint<34>");
  if (variant == 3) replace("hReady flip:", "hReady:");
  if (variant == 4) replace("len: uint<8>", "len: uint<7>");
  if (variant == 5) replace("region: uint<4>", "region: uint<3>");
  if (variant == 6) replace("data: uint<64>", "data: uint<63>");
  if (variant == 7) replace("in %hostClock: !firrtl.clock", "in %hostClock: !firrtl.uint<1>");
  if (variant == 8) replace("out %fased_ingress_reset", "in %fased_ingress_reset");
  if (variant == 9) replace("out %other", "out %fased_ingress_aw_deq");
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  auto inner = *c.getOps<FModuleOp>().begin();
  if (variant == 10) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), inner, "used"); }
  if (variant == 11) { b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFASEDIngressAW"), ConventionAttr::get(&context, Convention::Internal), SmallVector<PortInfo>{}); }
  SmallVector<Attribute> annos;
  for (auto name : {"fased_ingress.hBits.aw.bits.addr", "other", "fased_ingress_reset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGFASEDHostOutstandingWrapper|GGFASEDHostOutstandingWrapper>" + std::string(name)))}));
  if (variant != 12) c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDIngressAWQueue(c, error)), error);
  require(succeeded(verify(*root)), "queue IR verification failed");
  Interpreter sim(named(c, "GGFASEDIngressAWQueue10")); std::deque<APInt> expected;
  std::mt19937_64 random(209);
  unsigned pushes = 0, pops = 0, fullBlocked = 0, emptyBlocked = 0, resetWrites = 0, simultaneous = 0, wraps = 0, cycles = 0;
  auto request = [&]() { Request r; for (unsigned i = 0; i < 11; ++i) r.values[i] = random() & ((uint64_t(1) << widths[i])-1); return r; };
  auto cycle = [&](bool reset, bool valid, bool ready, Request data) {
    ++cycles; sim.memo.clear();
    auto input = [&](Value root, llvm::StringRef field, unsigned width, uint64_t value) { sim.memo.insert_or_assign(sim.key(root) + "." + field.str(), APInt(width, value)); };
    sim.memo.insert_or_assign(sim.key(sim.arg(1)), APInt(1, reset));
    input(sim.arg(2), "valid", 1, valid);
    for (unsigned i = 0; i < 11; ++i) input(sim.arg(2), "bits." + fields[i].str(), widths[i], data.values[i]);
    input(sim.arg(3), "ready", 1, ready);
    bool enqReady = !sim.drive(sim.arg(2), "ready").isZero(), deqValid = !sim.drive(sim.arg(3), "valid").isZero();
    require(enqReady == (expected.size() < 10) && deqValid == !expected.empty(), "queue occupancy differs");
    auto output = [&]() { Request r; for (unsigned i = 0; i < 11; ++i) r.values[i] = sim.drive(sim.arg(3), "bits." + fields[i].str()).getZExtValue(); return r.pack(); };
    if (deqValid) require(output() == expected.front(), "request field order differs");
    unsigned readAddress = sim.drive(sim.ram.getResult(0), "addr").getZExtValue();
    require(readAddress < 10 && output() == sim.memory.at(readAddress), "async read differs even when invalid");
    bool push = valid && enqReady, pop = ready && deqValid;
    fullBlocked += !enqReady && valid && ready; emptyBlocked += !deqValid && valid && ready;
    simultaneous += push && pop; pushes += push; pops += pop; resetWrites += reset && push;
    auto oldMemory = sim.memory;
    unsigned address = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    require(address < 10, "enqueue pointer escaped queue depth");
    if (pop) expected.pop_front(); if (push) expected.push_back(data.pack()); if (reset) expected.clear();
    if (push) oldMemory.at(address) = data.pack();
    sim.edge(); require(sim.memory == oldMemory, "reset clears RAM or suppresses accepted write"); sim.memo.clear();
    unsigned nextAddress = sim.drive(sim.ram.getResult(1), "addr").getZExtValue();
    if (!reset && push && address == 9) { require(nextAddress == 0, "non-power-of-two pointer wrap differs"); ++wraps; }
    require(output() == sim.memory.at(sim.drive(sim.ram.getResult(0), "addr").getZExtValue()), "post-edge asynchronous output differs");
  };
  cycle(true, false, false, request());
  for (unsigned round = 0; round < 12; ++round) {
    for (unsigned i = 0; i < 15; ++i) cycle(false, true, false, request());
    cycle(false, true, true, request()); // Full: accepted pop cannot enable same-cycle push.
    for (unsigned i = 0; i < 15; ++i) cycle(false, false, true, request());
    cycle(false, true, true, request()); // Empty: enqueued word cannot flow through.
  }
  cycle(true, true, true, request());
  for (unsigned i = 0; i < 20000; ++i) {
    bool fill = i % 1500 < 750;
    cycle(i % 997 == 0, fill || bool(random() & 1), !fill || bool(random() & 1), request());
  }
  require(pushes > 1000 && pops > 1000 && fullBlocked && emptyBlocked && resetWrites && simultaneous && wraps > 100,
          "queue boundary/reset/non-power-of-two wrap coverage missing");
  llvm::outs() << "FASED AW ingress queue: " << cycles << " cycles, " << wraps << " wrap events passed\n";
}
struct GateInterpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  GateInterpreter(FModuleOp m) : module(m) {
    auto link = [&](Value d, Value z) {
      if (!isa<BundleType>(d.getType())) { require(drivers.emplace(key(d), z).second, "multiple drivers"); return; }
      std::function<void(std::string,std::string,Type,bool)> expand = [&](std::string dest, std::string src, Type t, bool flip) {
        if (auto bundle = dyn_cast<BundleType>(t)) {
          for (auto e : bundle.getElements()) expand(dest + "." + e.name.str(), src + "." + e.name.str(), e.type, flip != e.isFlip);
        } else require(links.emplace(flip ? src : dest, flip ? dest : src).second, "multiple aggregate drivers");
      };
      expand(key(d), key(z), d.getType(), false);
    };
    for (auto c : m.getOps<StrictConnectOp>()) link(c.getDest(), c.getSrc());
    for (auto c : m.getOps<ConnectOp>()) link(c.getDest(), c.getSrc());
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
    if (links.count(k)) return read(links.at(k), w);
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
    else if (isa_and_nonnull<SubfieldOp>(op)) n = read(k, w);
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
};void gates(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDIngressAWQueue(c, error)), error);
  GateInterpreter sim(named(c, "GGFASEDIngressAW")); auto fifo = *sim.module.getOps<InstanceOp>().begin();
  std::mt19937_64 rng(209);
  for (unsigned flags = 0; flags < 256; ++flags) {
    sim.memo.clear(); auto f = [&](unsigned i) { return flags >> i & 1; };
    sim.put(sim.arg(1), "", f(7)); sim.put(sim.arg(2), "hValid", f(0));
    sim.put(fifo.getResult(2), "ready", f(1)); sim.put(sim.arg(4), "ready", f(2)); sim.put(sim.arg(5), "ready", f(3));
    const char *channels[]{"aw", "w", "ar"};
    for (auto [i, name] : llvm::enumerate(channels)) sim.put(sim.arg(2), "hBits." + std::string(name) + ".valid", f(4+i));
    require(sim.output(sim.arg(2), "hReady").getBoolValue() == bool(f(1)&&f(2)&&f(3)), "ingress capacity differs");
    require(sim.output(fifo.getResult(2), "valid").getBoolValue() == bool(f(0)&&f(2)&&f(3)&&f(4)), "AW valid must exclude own capacity");
    require(sim.output(sim.arg(4), "valid").getBoolValue() == bool(f(0)&&f(1)&&f(3)&&f(5)), "W valid must exclude own capacity");
    require(sim.output(sim.arg(5), "valid").getBoolValue() == bool(f(0)&&f(1)&&f(2)&&f(6)), "AR valid must exclude own capacity");
    require(sim.output(sim.arg(6), "").getBoolValue() == bool(f(0)&&f(1)&&f(2)&&f(3)&&f(4)), "AW acceptance differs");
    for (auto name : {"aw", "w", "ar"}) {
      auto channel = cast<BundleType>(cast<BundleType>(cast<BundleType>(sim.arg(2).getType()).getElement("hBits")->type).getElement(name)->type);
      auto bits = cast<BundleType>(channel.getElement("bits")->type);
      for (auto e : bits.getElements()) {
        unsigned width = *cast<UIntType>(e.type).getWidth(); uint64_t n = rng(); if (width < 64) n &= (uint64_t(1)<<width)-1;
        sim.put(sim.arg(2), "hBits." + std::string(name) + ".bits." + e.name.str(), n);
        Value output = name == llvm::StringRef("aw") ? fifo.getResult(2) : sim.arg(name == llvm::StringRef("w") ? 4 : 5);
        require(sim.output(output, "bits." + e.name.str()).getZExtValue() == n, "payload changed by capacity gate");
      }
    }
  }
  auto top = named(c, "GGFASEDIngressAWWrapper");
  require(top.getNumPorts() == 8, "wrapper port count differs");
  InstanceOp inner, gate; for (auto i : top.getOps<InstanceOp>()) { if (i.getName()=="sim") inner=i; else gate=i; }
  auto strict = [&](Value d, Value z) { for (auto conn : top.getOps<StrictConnectOp>()) if (conn.getDest()==d && conn.getSrc()==z) return true; return false; };
  auto bulk = [&](Value d, Value z) { for (auto conn : top.getOps<ConnectOp>()) if (conn.getDest()==d && conn.getSrc()==z) return true; return false; };
  auto arg = [&](unsigned i) { return top.getBodyBlock()->getArgument(i); };
  require(strict(gate.getResult(0),arg(0)) && strict(gate.getResult(1),inner.getResult(3)), "host clock or qualified ingress reset binding differs");
  require(bulk(gate.getResult(2),inner.getResult(2)), "ingress boundary binding differs");
  for (unsigned i=0; i<4; ++i) require(bulk(arg(4+i),gate.getResult(3+i)), "queue boundary binding differs");
  require(bulk(inner.getResult(0),arg(0)) && bulk(inner.getResult(1),arg(1)) && bulk(arg(2),inner.getResult(3)) && bulk(arg(3),inner.getResult(4)), "copied ports differ");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  const llvm::StringRef targets[]{"~GGFASEDIngressAWWrapper|GGFASEDHostOutstandingWrapper>fased_ingress.hBits.aw.bits.addr",
      "~GGFASEDIngressAWWrapper|GGFASEDIngressAWWrapper>other", "~GGFASEDIngressAWWrapper|GGFASEDIngressAWWrapper>fased_ingress_reset"};
  require(annos.size()==3,"annotation classes lost");
  for (unsigned i=0; i<3; ++i) require(cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue()==targets[i],"target transfer differs");
  llvm::outs() << "256 enqueue gate/payload cases and wrapper/annotation mapping passed\n";
}
void rejection(MLIRContext &ctx) {
  for (unsigned mode = 1; mode <= 12; ++mode) {
    auto root = fixture(ctx, mode); auto c = *root->getOps<CircuitOp>().begin(); std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addFASEDIngressAWQueue(c, error)) && !error.empty(), "malformed boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejection mutated circuit");
  }
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDIngressAWQueue(c, error)), error);
  require(failed(goldengate::addFASEDIngressAWQueue(c, error)), "repeated pass accepted");
  llvm::outs() << "12 atomic preflight rejections and repeated pass rejection passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); gates(ctx); rejection(ctx); } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
  return 0;
}
