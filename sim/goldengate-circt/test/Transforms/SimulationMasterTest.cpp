// See LICENSE for license details.
#include "goldengate/SimulationMaster.h"
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
struct BankInterpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  BankInterpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = cast<UIntType>(r.getResult().getType()).getWidthOrSentinel() == 64 ? 0xFEDCBA9876543210ULL : 1; }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 1 && reset == 4, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    memo[k] = n; return n;
  }
  void edge() {
    llvm::DenseMap<Value, uint64_t> next;
    for (auto r : module.getOps<RegOp>()) next[r.getResult()] = eval(drivers.at(key(r.getResult())));
    for (auto r : module.getOps<RegResetOp>())
      next[r.getResult()] = eval(r.getResetSignal()) ? eval(r.getResetValue()) : eval(drivers.at(key(r.getResult())));
    state = std::move(next);
  }
};
std::string dump(Operation *op) { std::string s; llvm::raw_string_ostream out(s); op->print(out); return s; }
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx, unsigned bad = 0) {
  std::string ports = "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>";
  if (bad == 1) ports = "in %hostReset: !firrtl.uint<1>";
  if (bad == 2) ports = "in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<2>";
  if (bad == 3) ports = "out %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>";
  if (bad == 4) ports += ", out %simulationMaster_mcr: !firrtl.uint<1>";
  auto root = parseSourceString<ModuleOp>("module { firrtl.circuit \"GGControlWriteTrackerWrapper\" { firrtl.module @GGControlWriteTrackerWrapper(" + ports + ") {} } }", &ctx);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder builder(&ctx);
  if (bad != 5) c->setAttr("rawAnnotations", builder.getArrayAttr({builder.getDictionaryAttr({
      builder.getNamedAttr("class", builder.getStringAttr("test.Target")),
      builder.getNamedAttr("target", builder.getStringAttr("~GGControlWriteTrackerWrapper|GGControlWriteTrackerWrapper>other"))})}));
  if (bad == 6) {
    builder.setInsertionPointToEnd(c.getBodyBlock()); builder.create<FModuleOp>(c.getLoc(), builder.getStringAttr("GGSimulationMasterBank"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
  }
  if (bad == 7) {
    auto top = named(c, "GGControlWriteTrackerWrapper"); builder.setInsertionPointToEnd(c.getBodyBlock());
    auto user = builder.create<FModuleOp>(c.getLoc(), builder.getStringAttr("User"), ConventionAttr::get(&ctx, Convention::Internal), ArrayRef<PortInfo>{});
    builder.setInsertionPointToStart(user.getBodyBlock()); builder.create<InstanceOp>(c.getLoc(), top, "top");
  }
  return root;
}
void test(MLIRContext &ctx) {
  auto root = fixture(ctx); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addSimulationMasterBank(c, error)), error);
  require(succeeded(verify(*root)), "invalid SimulationMaster IR");
  auto bank = named(c, "GGSimulationMasterBank"), top = named(c, "GGSimulationMasterWrapper");
  require(bank.getNumPorts() == 3 && top.getNumPorts() == 4, "incorrect MCR boundary");
  auto catalog = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(catalog.size() == 3, "wrong catalog size");
  unsigned idx = 0;
  for (auto a : catalog) {
    auto d = cast<DictionaryAttr>(a);
    require(d.getAs<IntegerAttr>("offset").getInt() == 4 * idx++ &&
        d.getAs<BoolAttr>("readable").getValue() && d.getAs<BoolAttr>("writeable").getValue(), "MCR register permissions/offsets differ");
  }
  BankInterpreter sim(bank); std::mt19937_64 rng(186);
  uint32_t delay = 1, done = 1, fingerprint = 1, read = 1, write = 1;
  unsigned cycles = 0, resetWrites = 0, sampleOverrides = 0;
  auto sample = [&](bool reset, unsigned mask) {
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(2)) + ".wstrb"] = rng() % 16;
    std::array<uint32_t, 3> payload, reads{done, read, write};
    for (unsigned i = 0; i < 3; ++i) {
      std::string rd = "read[" + std::to_string(i) + "]", wr = "write[" + std::to_string(i) + "]";
      payload[i] = rng(); sim.memo[sim.key(sim.arg(2)) + "." + wr + ".bits"] = payload[i];
      sim.memo[sim.key(sim.arg(2)) + "." + wr + ".valid"] = bool(mask & (1 << i));
      sim.memo[sim.key(sim.arg(2)) + "." + rd + ".ready"] = rng() & 1;
      require(sim.output(2, rd + ".bits") == reads[i] && sim.output(2, rd + ".valid") == 1 && sim.output(2, wr + ".ready") == 1, "MCR pre-edge data/handshake differs");
    }
    bool expired = delay == 0;
    read = mask & 2 ? payload[1] : fingerprint;
    fingerprint = reset ? 0x46697265 : write;
    write = reset ? 0x46697265 : mask & 4 ? payload[2] : write;
    done = reset ? 0 : mask & 1 ? payload[0] : expired;
    delay = reset ? 64 : expired ? delay : delay - 1;
    sim.edge();
    std::map<std::string, uint32_t> expected{{"initDelay", delay}, {"INIT_DONE", done}, {"rFingerprint", fingerprint}, {"PRESENCE_READ", read}, {"PRESENCE_WRITE", write}};
    for (auto r : bank.getOps<RegOp>()) require(sim.state.lookup(r.getResult()) == expected.at(r.getName().str()), "unreset sample priority differs");
    for (auto r : bank.getOps<RegResetOp>()) require(sim.state.lookup(r.getResult()) == expected.at(r.getName().str()), "reset/sample priority differs");
    resetWrites += reset && mask; sampleOverrides += mask & 3; ++cycles;
  };
  sample(true, 0);
  for (unsigned i = 0; i < 64; ++i) { sample(false, 0); require(done == 0, "INIT_DONE asserted early"); }
  sample(false, 0); require(done == 1, "INIT_DONE latency differs");
  for (unsigned i = 0; i < 4096; ++i) sample(i % 17 == 0, i % 8);
  require(resetWrites && sampleOverrides, "missing override coverage");
  auto anno = cast<DictionaryAttr>(c->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  require(anno.getAs<StringAttr>("target") == "~GGSimulationMasterWrapper|GGSimulationMasterWrapper>other", "copied target not transferred");
  auto before = dump(*root); require(failed(goldengate::addSimulationMasterBank(c, error)) && dump(*root) == before, "repeat mutated IR");
  for (unsigned i = 1; i <= 7; ++i) {
    auto r = fixture(ctx, i); auto ci = *r->getOps<CircuitOp>().begin(); auto before = dump(*r);
    require(failed(goldengate::addSimulationMasterBank(ci, error)) && dump(*r) == before, "malformed boundary accepted or mutated");
  }
  llvm::outs() << "SimulationMaster: " << cycles << " delay/sample/reset/write cycles and eight atomic rejections passed\n";
}
}
int main() {
  MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { test(ctx); return 0; } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
