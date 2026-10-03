// See LICENSE for license details.
#include "goldengate/ControlAddressDecode.h"
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
    for (auto r : m.getOps<RegOp>()) state[r.getResult()] = 0;
    for (auto r : m.getOps<RegResetOp>()) state[r.getResult()] = 0;
  }
  Value arg(unsigned i) { return module.getBodyBlock()->getArgument(i); }
  uint64_t output(unsigned i, llvm::StringRef field) { return eval(drivers.at(key(arg(i)) + "." + field.str())); }
  uint64_t eval(Value v) {
    auto k = key(v); if (memo.count(k)) return memo.at(k);
    auto *op = v.getDefiningOp(); uint64_t n;
    if (isa_and_nonnull<RegOp, RegResetOp>(op)) n = state.lookup(v);
    else if (drivers.count(k)) n = eval(drivers.at(k));
    else if (auto c = dyn_cast_or_null<ConstantOp>(op)) n = c.getValue().getZExtValue();
    else if (isa_and_nonnull<AddPrimOp>(op)) n = eval(op->getOperand(0)) + eval(op->getOperand(1));
    else if (isa_and_nonnull<SubPrimOp>(op)) n = eval(op->getOperand(0)) - eval(op->getOperand(1));
    else if (isa_and_nonnull<AndPrimOp>(op)) n = eval(op->getOperand(0)) & eval(op->getOperand(1));
    else if (isa_and_nonnull<XorPrimOp>(op)) n = eval(op->getOperand(0)) ^ eval(op->getOperand(1));
    else if (isa_and_nonnull<EQPrimOp>(op)) n = eval(op->getOperand(0)) == eval(op->getOperand(1));
    else if (isa_and_nonnull<LTPrimOp>(op)) n = eval(op->getOperand(0)) < eval(op->getOperand(1));
    else if (isa_and_nonnull<GTPrimOp>(op)) n = eval(op->getOperand(0)) > eval(op->getOperand(1));
    else if (isa_and_nonnull<NotPrimOp>(op)) n = !eval(op->getOperand(0));
    else if (isa_and_nonnull<CatPrimOp>(op)) n = (eval(op->getOperand(0)) << cast<UIntType>(op->getOperand(1).getType()).getWidthOrSentinel()) | eval(op->getOperand(1));
    else if (isa_and_nonnull<PadPrimOp>(op)) n = eval(op->getOperand(0));
    else if (isa_and_nonnull<MuxPrimOp>(op)) n = eval(op->getOperand(eval(op->getOperand(0)) ? 1 : 2));
    else if (auto bits = dyn_cast_or_null<BitsPrimOp>(op))
      n = (eval(bits.getInput()) >> bits.getLo()) & ((uint64_t(1) << (bits.getHi() - bits.getLo() + 1)) - 1);
    else throw std::runtime_error("unsupported operation or missing driver");
    unsigned width = cast<UIntType>(v.getType()).getWidthOrSentinel();
    if (width < 64) n &= (uint64_t(1) << width) - 1;
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
  auto root = parseSourceString<ModuleOp>(R"(module { firrtl.circuit "GGControlErrorWrapper" {
    firrtl.module @GGControlErrorWrapper(in %hostClock: !firrtl.clock,
      in %hostReset: !firrtl.uint<1>, out %other: !firrtl.uint<8>) {}
  } })", &context);
  require(bool(root), "fixture parse"); auto c = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto name : {"hostClock", "hostReset", "other"}) annotations.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Annotation")),
      b.getNamedAttr("target", b.getStringAttr("~GGControlErrorWrapper|GGControlErrorWrapper>" + std::string(name)))}));
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Internal")),
      b.getNamedAttr("targets", b.getArrayAttr({b.getStringAttr("~GGControlErrorWrapper"),
          b.getStringAttr("~GGControlErrorWrapper|Model>state")}))}));
  c->setAttr("rawAnnotations", b.getArrayAttr(annotations)); return root;
}
const goldengate::ControlMMIORegion regions[]{
    {"BlockDevBridgeModule_0", 0, 128}, {"FASEDMemoryTimingModel_0", 128, 128},
    {"TracerVBridgeModule_0", 256, 64}, {"TSIBridgeModule_0", 320, 64},
    {"LoadMemWidget_0", 384, 64}, {"PeekPokeBridgeModule_0", 448, 32},
    {"UARTBridgeModule_0", 480, 32}, {"ClockBridgeModule_0", 512, 32},
    {"SimulationMaster_0", 544, 16}, {"ResetPulseBridgeModule_0", 560, 8},
    {"CPUManagedStreamEngine_0", 568, 4}};
unsigned samples = 0;
void checkMap(MLIRContext &context, unsigned width,
              llvm::ArrayRef<goldengate::ControlMMIORegion> entries) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlAddressDecode(c, width, entries, error)), error);
  require(succeeded(verify(*root)), "decoder invalid");
  auto helper = named(c, "GGControlAddressDecode"); Interpreter sim(helper);
  require(helper.getOps<RegOp>().empty() && helper.getOps<RegResetOp>().empty() &&
      helper.getOps<MemOp>().empty(), "decoder must be combinational");
  auto catalog = helper->getAttrOfType<ArrayAttr>("goldengate.controlRegions");
  require(catalog && catalog.size() == entries.size(), "region catalog lost");
  for (auto [i, entry] : llvm::enumerate(entries)) {
    auto row = cast<DictionaryAttr>(catalog[i]);
    require(row.getAs<StringAttr>("name").getValue() == entry.name &&
        row.getAs<IntegerAttr>("start").getValue().getZExtValue() == entry.start &&
        row.getAs<IntegerAttr>("size").getValue().getZExtValue() == entry.size &&
        row.getAs<IntegerAttr>("slave").getInt() == int64_t(i), "region catalog differs");
  }
  uint64_t mask = (uint64_t(1) << width) - 1;
  auto sample = [&](uint64_t aw, uint64_t ar) {
    ++samples; sim.memo.clear(); sim.memo[sim.key(sim.arg(0))] = aw; sim.memo[sim.key(sim.arg(1))] = ar;
    for (unsigned ch = 0; ch < 2; ++ch) {
      uint64_t address = ch ? ar : aw, onehot = 0; unsigned target = entries.size();
      for (auto [i, region] : llvm::enumerate(entries))
        if (address >= region.start && address - region.start < region.size) {
          onehot = uint64_t(1) << i; target = i;
        }
      require(sim.eval(sim.arg(2 + ch)) == onehot && sim.eval(sim.arg(4 + ch)) == target &&
          sim.eval(sim.arg(6 + ch)) == (onehot == 0), "region selection or error encoding differs");
    }
  };
  for (auto region : entries) {
    for (uint64_t delta = 0; delta < std::min(uint64_t(1024), region.size); ++delta)
      sample(region.start + delta, region.start + region.size - 1 - delta);
    for (auto address : {region.start, region.start + region.size - 1,
                         (region.start + region.size) & mask, (region.start - 1) & mask})
      sample(address, mask - address);
  }
  std::mt19937_64 random(176);
  for (unsigned i = 0; i < 20000; ++i) sample(random() & mask, random() & mask);
}
void mapping(MLIRContext &context) {
  auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); std::string error;
  require(succeeded(goldengate::addControlAddressDecode(c, 25, regions, error)), error);
  auto wrapper = named(c, "GGControlDecodeWrapper"), helper = named(c, "GGControlAddressDecode");
  require(wrapper.getNumPorts() == 11 && helper.getNumPorts() == 8, "wrong decoder boundary");
  Interpreter keys(helper); std::map<std::string, Value> wires; InstanceOp decoder, sim;
  for (auto instance : wrapper.getOps<InstanceOp>()) {
    if (instance.getName() == "controlDecode") decoder = instance;
    if (instance.getName() == "sim") sim = instance;
  }
  for (auto connect : wrapper.getOps<StrictConnectOp>()) wires.emplace(keys.key(connect.getDest()), connect.getSrc());
  for (unsigned i = 0; i < helper.getNumPorts(); ++i) {
    Value external = wrapper.getBodyBlock()->getArgument(i + 3);
    require(wires.at(keys.key(i < 2 ? decoder.getResult(i) : external)) ==
        (i < 2 ? external : decoder.getResult(i)), "decoder binding differs");
    require(wrapper.getPortName(i + 3) == "ctrl_decode_" + helper.getPortName(i).str(), "decoder field name differs");
  }
  unsigned copied = 0;
  for (auto connect : wrapper.getOps<ConnectOp>()) {
    unsigned i = copied++;
    require(connect.getDest() == (i < 2 ? sim.getResult(i) : wrapper.getBodyBlock()->getArgument(i)) &&
        connect.getSrc() == (i < 2 ? wrapper.getBodyBlock()->getArgument(i) : sim.getResult(i)), "copied port binding differs");
  }
  require(copied == 3, "copied port missing");
  auto raw = c->getAttrOfType<ArrayAttr>("rawAnnotations"); require(raw.size() == 4, "annotations lost");
  for (unsigned i = 0; i < 3; ++i)
    require(cast<DictionaryAttr>(raw[i]).getAs<StringAttr>("target").getValue() ==
        "~GGControlDecodeWrapper|GGControlDecodeWrapper>" + wrapper.getPortName(i).str(), "copied target transfer differs");
  auto targets = cast<DictionaryAttr>(raw[3]).getAs<ArrayAttr>("targets");
  require(cast<StringAttr>(targets[0]).getValue() == "~GGControlDecodeWrapper" &&
      cast<StringAttr>(targets[1]).getValue() == "~GGControlDecodeWrapper|Model>state", "internal identity transfer differs");
}
void rejection(MLIRContext &context) {
  for (unsigned bad = 0; bad < 15; ++bad) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); auto top = named(c, "GGControlErrorWrapper");
    OpBuilder b(&context); unsigned address = 25;
    SmallVector<goldengate::ControlMMIORegion> entries(std::begin(regions), std::end(regions));
    if (bad == 0) c.setName("WrongTop"); if (bad == 1) c->removeAttr("rawAnnotations");
    if (bad == 2) {
      SmallVector<Attribute> names(top.getPortNames().begin(), top.getPortNames().end());
      names[2] = b.getStringAttr("ctrl_decode_aw_addr"); top.setPortNames(names);
    }
    if (bad == 3) { b.setInsertionPointToStart(top.getBodyBlock()); b.create<InstanceOp>(c.getLoc(), top, "used"); }
    if (bad == 4 || bad == 5) {
      b.setInsertionPointToEnd(c.getBodyBlock()); b.create<FModuleOp>(c.getLoc(), b.getStringAttr(
          bad == 4 ? "GGControlAddressDecode" : "GGControlDecodeWrapper"), top.getConventionAttr(), ArrayRef<PortInfo>{});
    }
    if (bad == 6) address = 0; if (bad == 7) address = 64;
    if (bad == 8) entries.clear(); if (bad == 9) entries[0].size = 0;
    if (bad == 10) entries[0].start = uint64_t(1) << address;
    if (bad == 11) entries[0].size = UINT64_MAX;
    if (bad == 12) entries[0].name = "";
    if (bad == 13) entries[1].name = entries[0].name;
    if (bad == 14) entries[1].start = 0;
    std::string before, after, error; { llvm::raw_string_ostream out(before); root->print(out); }
    require(failed(goldengate::addControlAddressDecode(c, address, entries, error)), "bad decoder accepted");
    { llvm::raw_string_ostream out(after); root->print(out); } require(before == after, "rejection mutated IR");
  }
}
}
int main() {
  try { MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    checkMap(context, 25, regions);
    // Nonzero base, holes, unsorted slave indices, and the full address-space
    // endpoint exercise the generic API independently of the U250 catalog.
    const goldengate::ControlMMIORegion sparse[]{{"high", 100, 10}, {"low", 4, 4}};
    const goldengate::ControlMMIORegion full[]{{"all", 0, uint64_t(1) << 63}};
    checkMap(context, 8, sparse); checkMap(context, 63, full);
    mapping(context); rejection(context);
    llvm::outs() << "Control decoder: " << samples << " address pairs; catalog, wiring, targets and 15 atomic rejections passed\n";
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
