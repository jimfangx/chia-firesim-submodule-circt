// See LICENSE for license details.
#include "goldengate/FASEDHostOutstanding.h"
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
#include <functional>
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
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned mode = 0) {
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGFASEDTokenWrapper\" { firrtl.module @GGFASEDTokenWrapper() {} } }", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin();
  (*c.getOps<FModuleOp>().begin()).erase();
  OpBuilder b(c.getBodyBlock(), c.getBodyBlock()->begin()); auto bit = UIntType::get(&ctx, 1, false);
  auto readiness = BundleType::get(&ctx, {{b.getStringAttr("readValid"), false, bit},
      {b.getStringAttr("writeValid"), false, bit}, {b.getStringAttr("hostMemIdle"), false, bit}});
  SmallVector<PortInfo> ports{{b.getStringAttr("hostClock"), ClockType::get(&ctx), Direction::In},
      {b.getStringAttr("hostReset"), bit, Direction::In},
      {b.getStringAttr("fased_readiness"), readiness, Direction::In},
      {b.getStringAttr("other"), UIntType::get(&ctx, 8, false), Direction::Out}};
  if (mode == 1) ports[2].name = b.getStringAttr("missing");
  if (mode == 2) ports[1].type = UIntType::get(&ctx, 2, false);
  if (mode == 3) ports[2].direction = Direction::Out;
  if (mode == 4) ports[3].name = b.getStringAttr("fased_host_mem_idle");
  auto inner = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(c.getName()), ConventionAttr::get(&ctx, Convention::Internal), ports);
  if (mode != 5) {
    auto engine = b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFASEDTokenEngine"), ConventionAttr::get(&ctx, Convention::Internal), SmallVector<PortInfo>{});
    if (mode != 6) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
        b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI64IntegerAttr(mode == 7 ? 8 : 10))}))}));
  }
  if (mode == 8) b.create<FModuleOp>(c.getLoc(), b.getStringAttr("GGFASEDHostOutstanding"), ConventionAttr::get(&ctx, Convention::Internal), SmallVector<PortInfo>{});
  if (mode == 9) { b.setInsertionPointToStart(inner.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), inner, "used"); }
  if (mode == 10) c.setName("other");
  SmallVector<Attribute> raw;
  for (auto suffix : {"", "|GGFASEDTokenWrapper>other", "|GGFASEDTokenWrapper>fased_readiness",
      "|GGFASEDTokenWrapper>fased_readiness.readValid", "|GGFASEDTokenWrapper>fased_readiness.writeValid",
      "|GGFASEDTokenWrapper>fased_readiness.hostMemIdle", "|GGFASEDTokenEngine"})
    raw.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr(std::string("~GGFASEDTokenWrapper") + suffix))}));
  if (mode != 11) c->setAttr("rawAnnotations", b.getArrayAttr(raw));
  return root;
}
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, std::string> links;
  std::map<std::string, APInt> memo;
  llvm::DenseMap<Value, APInt> state;
  Interpreter(FModuleOp m) : module(m) {
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
};
void behavior(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDHostOutstanding(c, error)), error);
  require(succeeded(verify(*root)), "invalid FASED host outstanding IR");
  Interpreter sim(named(c, "GGFASEDHostOutstanding")); unsigned cases = 0;
  SmallVector<Value> state; for (auto r : sim.module.getOps<RegResetOp>()) state.push_back(r.getResult());
  require(state.size() == 2, "host counter state missing");
  const llvm::StringRef inputs[]{"arReady","arValid","rReady","rValid","rLast","awReady","awValid","bReady","bValid"};
  for (unsigned reads = 0; reads < 16; ++reads) for (unsigned writes = 0; writes < 16; ++writes)
    for (unsigned flags = 0; flags < 1024; ++flags) {
      sim.memo.clear(); sim.state[state[0]] = APInt(4, reads); sim.state[state[1]] = APInt(4, writes);
      auto f = [&](unsigned i) { return (flags >> i) & 1; };
      sim.put(sim.arg(1), "", f(9)); for (unsigned i = 0; i < 9; ++i) sim.put(sim.arg(2), inputs[i], f(i));
      require(sim.output(sim.arg(3), "").getBoolValue() == (!reads && !writes), "idle differs");
      require(sim.output(sim.arg(4), "").getBoolValue() == (reads != 0), "read inflight differs");
      auto next = [&](unsigned n, bool inc, bool dec) {
        if (f(9)) return 0u;
        if (inc == dec) return n;
        if (inc && n < 10) return n + 1;
        if (dec && n > 0) return n - 1;
        return n;
      };
      sim.edge();
      require(sim.state[state[0]].getZExtValue() == next(reads, f(0)&&f(1), f(2)&&f(3)&&f(4)), "read counter differs");
      require(sim.state[state[1]].getZExtValue() == next(writes, f(5)&&f(6), f(7)&&f(8)), "write counter differs");
      ++cases;
    }
  auto wrapper = named(c, "GGFASEDHostOutstandingWrapper"); Interpreter wired(wrapper);
  InstanceOp inner, counter; for (auto i : wrapper.getOps<InstanceOp>()) { if (i.getName() == "sim") inner = i; else counter = i; }
  require(wrapper.getPorts().size() == 7, "wrapper boundary port count differs");
  for (unsigned flags = 0; flags < 8192; ++flags) {
    wired.memo.clear();
    auto f = [&](unsigned i) { return flags >> i & 1; };
    wired.put(wired.arg(1), "", f(0));
    wired.put(wired.arg(3), "readValid", f(1)); wired.put(wired.arg(3), "writeValid", f(2));
    wired.put(counter.getResult(3), "", f(3)); wired.put(counter.getResult(4), "", f(4));
    for (unsigned i = 0; i < 9; ++i) wired.put(wired.arg(4), inputs[i], f(i));
    require(wired.output(counter.getResult(1), "").getBoolValue() == bool(f(0)), "counter reset is not host reset");
    for (unsigned i = 0; i < 9; ++i) require(wired.output(counter.getResult(2), inputs[i]).getBoolValue() == bool(f(i)), "host handshake wire differs");
    require(wired.output(inner.getResult(2), "hostMemIdle").getBoolValue() == bool(f(3)), "token idle gate differs");
    require(wired.output(inner.getResult(2), "readValid").getBoolValue() == bool(f(1)), "read readiness differs");
    require(wired.output(inner.getResult(2), "writeValid").getBoolValue() == bool(f(2)), "write readiness differs");
    require(wired.output(wired.arg(5), "").getBoolValue() == bool(f(3)) && wired.output(wired.arg(6), "").getBoolValue() == bool(f(4)), "host status boundary differs");
  }
  const llvm::StringRef targets[]{"~GGFASEDHostOutstandingWrapper", "~GGFASEDHostOutstandingWrapper|GGFASEDHostOutstandingWrapper>other",
      "~GGFASEDHostOutstandingWrapper|GGFASEDTokenWrapper>fased_readiness",
      "~GGFASEDHostOutstandingWrapper|GGFASEDHostOutstandingWrapper>fased_egress_readiness.readValid",
      "~GGFASEDHostOutstandingWrapper|GGFASEDHostOutstandingWrapper>fased_egress_readiness.writeValid",
      "~GGFASEDHostOutstandingWrapper|GGFASEDHostOutstandingWrapper>fased_host_mem_idle",
      "~GGFASEDHostOutstandingWrapper|GGFASEDTokenEngine"};
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(raw.size() == 7, "annotation classes lost");
  for (unsigned i = 0; i < 7; ++i) require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue() == targets[i], "annotation target differs");
  llvm::outs() << cases << " host counter transitions and 8192 boundary cases passed\n";
}
void rejection(MLIRContext &ctx) {
  for (unsigned mode = 1; mode <= 11; ++mode) {
    auto root = fixture(ctx, mode); auto c = *root->getOps<CircuitOp>().begin(); std::string before, after, error;
    { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addFASEDHostOutstanding(c, error)) && !error.empty(), "malformed boundary accepted");
    { llvm::raw_string_ostream out(after); root->print(out); }
    require(before == after, "rejection mutated circuit");
  }
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDHostOutstanding(c, error)), error);
  require(failed(goldengate::addFASEDHostOutstanding(c, error)), "repeated pass accepted");
  llvm::outs() << "11 atomic preflight rejections and repeated pass rejection passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { behavior(ctx); rejection(ctx); } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
  return 0;
}
