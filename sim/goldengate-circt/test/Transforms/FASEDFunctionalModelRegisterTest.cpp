// See LICENSE for license details.
#include "goldengate/FASEDFunctionalModelRegister.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
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
struct Interpreter {
  FModuleOp module;
  std::map<std::string, Value> drivers;
  std::map<std::string, uint64_t> memo;
  llvm::DenseMap<Value, uint64_t> state;
  std::string key(Value v) {
    if (auto f = v.getDefiningOp<SubfieldOp>()) return key(f.getInput()) + "." + f.getFieldName().str();
    if (auto f = v.getDefiningOp<SubindexOp>()) return key(f.getInput()) + "[" + std::to_string(f.getIndex()) + "]";
    return std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
  }
  Interpreter(FModuleOp m) : module(m) {
    for (auto c : m.getOps<StrictConnectOp>())
      require(drivers.emplace(key(c.getDest()), c.getSrc()).second, "multiple drivers");
    unsigned regular = 0, reset = 0;
    for (auto r : m.getOps<RegOp>()) { ++regular; state[r.getResult()] = 0xA5 & ((uint64_t(1) << cast<UIntType>(r.getResult().getType()).getWidthOrSentinel()) - 1); }
    for (auto r : m.getOps<RegResetOp>()) { ++reset; state[r.getResult()] = 1; }
    require(regular == 0 && reset == 1, "wrong bank reset policy");
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
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
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"(module {
    firrtl.circuit "GGFASEDLatencyRegistersWrapper" {
      firrtl.module @GGFASEDTokenEngine() {}
      firrtl.module @GGFASEDLatencyRegistersWrapper(
        in %hostClock: !firrtl.clock, in %hostReset: !firrtl.uint<1>,
        in %fased_ingress_relaxed: !firrtl.uint<1>,
        in %targetFire: !firrtl.uint<1>, in %modelReset: !firrtl.uint<1>,
        out %other: !firrtl.uint<8>) {}
    } })", &context);
  require(bool(root), "fixture parse failed"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  named(c, "GGFASEDTokenEngine")->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({
    b.getNamedAttr("axi4Edge", b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI32IntegerAttr(10))}))}));
  SmallVector<Attribute> annos;
  for (auto name : {"fased_ingress_relaxed", "other", "hostReset"})
    annos.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
        b.getNamedAttr("target", b.getStringAttr("~GGFASEDLatencyRegistersWrapper|GGFASEDLatencyRegistersWrapper>" + std::string(name)))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annos)); return root;
}
void behavior(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDFunctionalModelRegister(c, error)), error);
  require(succeeded(verify(*root)), "functional model register IR verification failed");
  Interpreter sim(named(c, "GGFASEDFunctionalModelRegister")); uint32_t expected = 1;
  std::mt19937 random(230); unsigned resetWrites = 0, zeroStrobeWrites = 0, highBits = 0;
  for (unsigned cycle = 0; cycle < 32768; ++cycle) {
    bool reset = cycle < 256 ? cycle & 1 : (random() & 31) == 0;
    bool write = cycle < 256 ? (cycle >> 1) & 1 : random() & 1;
    unsigned strobe = cycle < 256 ? (cycle >> 2) & 15 : random() & 15;
    const uint32_t edges[]{0, 1, 2, 3, 0x80000000U, 0x80000001U, UINT32_MAX, 65536};
    uint32_t data = cycle < 256 ? edges[(cycle >> 5) % 8] : random();
    sim.memo.clear(); sim.memo[sim.key(sim.arg(1))] = reset;
    sim.memo[sim.key(sim.arg(3)) + ".wstrb"] = strobe;
    sim.memo[sim.key(sim.arg(3)) + ".write[0].valid"] = write;
    sim.memo[sim.key(sim.arg(3)) + ".write[0].bits"] = data;
    sim.memo[sim.key(sim.arg(3)) + ".read[0].ready"] = random() & 1;
    auto check = [&]() {
      require(sim.output(3, "read[0].bits") == expected, "full word readback mismatch");
      require(sim.output(3, "read[0].valid") == 1 && sim.output(3, "write[0].ready") == 1,
          "MCR handshake mismatch");
      require(sim.eval(sim.arg(2)) == (expected & 1), "ingress must consume only bit zero");
    };
    check(); uint32_t next = reset ? 0 : write ? data : expected;
    resetWrites += reset && write; zeroStrobeWrites += !strobe && write; highBits += bool(data & 0x80000000U);
    sim.edge(); sim.memo.clear(); expected = next; check();
  }
  require(resetWrites && zeroStrobeWrites && highBits, "coverage missing");
}

void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addFASEDFunctionalModelRegister(c, error)), error);
  auto top = named(c, "GGFASEDFunctionalModelRegisterWrapper"), bank = named(c, "GGFASEDFunctionalModelRegister");
  require(top.getNumPorts() == 6 && top.getPortName(5) == "fased_functional_model_mcr", "relaxed input not consumed");
  auto regs = bank->getAttrOfType<ArrayAttr>("goldengate.mmioRegisters");
  require(regs && regs.size() == 1, "missing register map");
  auto d = cast<DictionaryAttr>(regs[0]);
  require(d.getAs<StringAttr>("name").getValue() == "relaxFunctionalModel" &&
      d.getAs<IntegerAttr>("offset").getInt() == 72 &&
      d.getAs<BoolAttr>("readable").getValue() && d.getAs<BoolAttr>("writeable").getValue(),
      "wrong global offset or permissions");
  InstanceOp sim, mmio; for (auto inst : top.getOps<InstanceOp>()) {
    if (inst.getName() == "sim") sim = inst; else mmio = inst;
  }
  unsigned bindings = 0;
  for (auto conn : top.getOps<StrictConnectOp>()) {
    if (conn.getDest() == mmio.getResult(0)) { require(conn.getSrc() == top.getArgument(0), "wrong host clock"); ++bindings; }
    if (conn.getDest() == mmio.getResult(1)) { require(conn.getSrc() == top.getArgument(1), "reset was target-gated"); ++bindings; }
    if (conn.getDest() == sim.getResult(2)) {
      require(conn.getSrc() == mmio.getResult(2), "relaxation binding mismatch"); ++bindings;
    }
  }
  require(bindings == 3, "missing host or ingress binding");
  auto annos = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(annos.size() == 3, "annotations lost");
  for (unsigned i = 0; i < 3; ++i) {
    auto target = cast<DictionaryAttr>(annos[i]).getAs<StringAttr>("target").getValue();
    require(target.starts_with("~GGFASEDFunctionalModelRegisterWrapper|") &&
        target.contains(i == 0 ? "|GGFASEDLatencyRegistersWrapper>" : "|GGFASEDFunctionalModelRegisterWrapper>"), "target identity lost");
  }
}

void rejections(MLIRContext &context) {
  for (unsigned mode = 0; mode < 14; ++mode) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
    auto top = named(c, "GGFASEDLatencyRegistersWrapper"), engine = named(c, "GGFASEDTokenEngine");
    if (mode == 0) c.setName("WrongTop");
    if (mode == 1) c->removeAttr("rawAnnotations");
    if (mode == 2) engine->removeAttr("goldengate.bridgeConstructor");
    if (mode == 3) engine->setAttr("goldengate.bridgeConstructor", b.getDictionaryAttr({b.getNamedAttr("axi4Edge",
        b.getDictionaryAttr({b.getNamedAttr("maxFlight", b.getI32IntegerAttr(9))}))}));
    if (mode >= 4 && mode < 7) { auto names = llvm::to_vector(top.getPortNames()); names[mode - 4] = b.getStringAttr("missing"); top.setPortNames(names); }
    if (mode >= 7 && mode < 10) {
      SmallVector<bool> dirs; for (auto port : top.getPorts()) dirs.push_back(port.direction == Direction::Out); dirs[mode - 7] = true; top.setPortDirections(dirs);
    }
    if (mode == 10) { b.setInsertionPointToStart(engine.getBodyBlock()); b.create<InstanceOp>(top.getLoc(), top, "usedTop"); }
    if (mode == 11) { auto names = llvm::to_vector(top.getPortNames()); names[5] = b.getStringAttr("fased_functional_model_mcr"); top.setPortNames(names); }
    if (mode == 12) {
      top.getArgument(2).setType(UIntType::get(&context, 32, false));
      auto types = llvm::to_vector(top.getPortTypes()); types[2] = TypeAttr::get(UIntType::get(&context, 32, false));
      top.setPortTypes(types);
    }
    if (mode == 13) {
      b.setInsertionPointToEnd(c.getBodyBlock());
      b.create<FModuleOp>(top.getLoc(), b.getStringAttr("GGFASEDFunctionalModelRegister"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
    }
    std::string before; llvm::raw_string_ostream out(before); root->print(out); out.flush(); std::string error;
    require(failed(goldengate::addFASEDFunctionalModelRegister(c, error)) && !error.empty(), "invalid boundary accepted");
    std::string after; llvm::raw_string_ostream next(after); root->print(next); next.flush();
    require(before == after, "rejection mutated circuit");
  }
}
}
int main() {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    behavior(context); mapping(context); rejections(context);
    llvm::outs() << "FASED functional model register: 32768 cycles, host/ingress bindings, targets and 14 atomic rejections passed\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << "\n"; return 1; }
}
