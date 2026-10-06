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
void allocation(MLIRContext &context) {
  // FPGATop registration order: master, BridgeIO annotations, LoadMem, stream.
  const goldengate::ControlMMIOWidget widgets[]{
      {"SimulationMaster_0", 3}, {"PeekPokeBridgeModule_0", 7},
      {"ResetPulseBridgeModule_0", 2}, {"BlockDevBridgeModule_0", 26},
      {"UARTBridgeModule_0", 6}, {"FASEDMemoryTimingModel_0", 21},
      {"TracerVBridgeModule_0", 15}, {"TSIBridgeModule_0", 9},
      {"ClockBridgeModule_0", 6}, {"LoadMemWidget_0", 9},
      {"CPUManagedStreamEngine_0", 1}};
  SmallVector<goldengate::ControlMMIORegion> allocated;
  std::string error;
  require(succeeded(goldengate::allocateControlMMIORegions(25, widgets, allocated, error)), error);
  require(allocated.size() == std::size(regions), "baseline allocation count");
  for (auto [i, expected] : llvm::enumerate(regions))
    require(allocated[i].name == expected.name && allocated[i].start == expected.start &&
        allocated[i].size == expected.size, "HasWidgets allocation differs from immutable U250 map");
  checkMap(context, 25, allocated);

  // Two synthesized Print widgets register after existing BridgeIO peers and
  // before LoadMem/stream. Six MMIO words round to 32 bytes, with stable ties.
  SmallVector<goldengate::ControlMMIOWidget> enabled(std::begin(widgets), std::end(widgets));
  enabled.insert(enabled.begin() + 9, {{"PrintBridgeModule_0", 6}, {"PrintBridgeModule_1", 6}});
  require(succeeded(goldengate::allocateControlMMIORegions(25, enabled, allocated, error)), error);
  require(allocated.size() == 13 && allocated[8].name == "PrintBridgeModule_0" &&
      allocated[8].start == 0x220 && allocated[8].size == 0x20 &&
      allocated[9].name == "PrintBridgeModule_1" && allocated[9].start == 0x240 &&
      allocated[10].name == "SimulationMaster_0" && allocated[10].start == 0x260 &&
      allocated[11].start == 0x270 && allocated[12].start == 0x278,
      "Print allocation did not shift smaller widgets in stable order");
  checkMap(context, 25, allocated);

  // Custom regions replace bank-derived size, including an empty custom bank.
  // Equal-size ties must follow input order, even when names sort differently.
  const goldengate::ControlMMIOWidget custom[]{
      {"z", 0, 8}, {"a", 1, 8}, {"large", 1, 32}, {"rounded", 3}};
  require(succeeded(goldengate::allocateControlMMIORegions(6, custom, allocated, error)), error);
  require(allocated[0].name == "large" && allocated[0].start == 0 &&
      allocated[1].name == "rounded" && allocated[1].start == 32 &&
      allocated[2].name == "z" && allocated[2].start == 48 &&
      allocated[3].name == "a" && allocated[3].start == 56, "custom/tie allocation differs");
  checkMap(context, 6, allocated);
  const goldengate::ControlMMIOWidget full[]{{"full", uint64_t(1) << 61}};
  require(succeeded(goldengate::allocateControlMMIORegions(63, full, allocated, error)) &&
      allocated[0].size == uint64_t(1) << 63, "full address space allocation failed");
  checkMap(context, 63, allocated);

  for (unsigned bad = 0; bad < 12; ++bad) {
    SmallVector<goldengate::ControlMMIOWidget> invalid{{"widget", 1}};
    unsigned width = 25;
    if (bad == 0) width = 0;
    if (bad == 1) width = 64;
    if (bad == 2) invalid.clear();
    if (bad == 3) invalid[0].name = "";
    if (bad == 4) invalid.push_back(invalid[0]);
    if (bad == 5) invalid[0].registerCount = 0;
    if (bad == 6) invalid[0].registerCount = UINT64_MAX;
    if (bad == 7) invalid[0].customSize = 0;
    if (bad == 8) invalid[0].customSize = 12;
    if (bad == 9) invalid[0].customSize = uint64_t(1) << 26;
    if (bad == 10) {
      width = 4; invalid = {{"first", 3}, {"second", 1}};
    }
    if (bad == 11) {
      width = 63; invalid[0].registerCount = (uint64_t(1) << 61) + 1;
    }
    SmallVector<goldengate::ControlMMIORegion> result{{"sentinel", 123, 456}};
    require(failed(goldengate::allocateControlMMIORegions(width, invalid, result, error)),
        "invalid allocation accepted");
    require(result.size() == 1 && result[0].name == "sentinel" &&
        result[0].start == 123 && result[0].size == 456, "failed allocation changed output");
  }
}
// Build a live CIRCT registry split across two sparse fragments. Deliberately
// reverse row and module order: allocation must follow widget registration,
// while register identity comes from offsets rather than visitation order.
void registry(MLIRContext &context) {
  const unsigned counts[]{1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63};
  auto setup = [&](CircuitOp c, unsigned count, unsigned fault) {
    OpBuilder b(&context); b.setInsertionPointToEnd(c.getBodyBlock());
    auto uint = [&](unsigned width) { return UIntType::get(&context, width); };
    auto token = BundleType::get(&context, {{b.getStringAttr("ready"), true, uint(1)},
        {b.getStringAttr("valid"), false, uint(1)},
        {b.getStringAttr("bits"), false, uint(fault == 13 ? 64 : 32)}});
    auto lanes = FVectorType::get(token, count + (fault == 14));
    auto mcr = BundleType::get(&context, {{b.getStringAttr("read"), false, lanes},
        {b.getStringAttr("write"), fault != 15, lanes},
        {b.getStringAttr("wstrb"), true, uint(fault == 16 ? 8 : 4)}});
    SmallVector<PortInfo> ports{{b.getStringAttr(fault == 17 ? "wrong" : "mcr"), mcr,
        fault == 18 ? Direction::Out : Direction::In}};
    if (fault != 19) b.create<FModuleOp>(c.getLoc(), b.getStringAttr("Adapter"),
        ConventionAttr::get(&context, Convention::Internal), ports);
    for (unsigned fragment = 0; fragment < (count == 1 ? 1u : 2u); ++fragment) {
      auto bank = b.create<FModuleOp>(c.getLoc(), b.getStringAttr(fragment ? "Odd" : "Even"),
          ConventionAttr::get(&context, Convention::Internal), ArrayRef<PortInfo>{});
      SmallVector<Attribute> rows;
      for (unsigned j = count; j-- > 0;) if (j % 2 == fragment) {
        unsigned offset = 4 * j;
        if (j == 0 && fault == 0) offset = 1;
        if (j == 0 && fault == 1) offset = 4;
        if (j == count - 1 && fault == 2) offset += 4;
        auto row = b.getDictionaryAttr({
            b.getNamedAttr("name", b.getStringAttr(j == 0 && fault == 3 ? "word1" :
                j == 0 && fault == 4 ? "" : "word" + std::to_string(j))),
            b.getNamedAttr("offset", b.getI64IntegerAttr(j == 0 && fault == 5 ? -4 : int64_t(offset))),
            b.getNamedAttr("readable", b.getBoolAttr(fault != 6)),
            b.getNamedAttr("writeable", b.getBoolAttr(false))});
        NamedAttrList attrs(row);
        if (j == 0 && fault == 7) attrs.erase("name");
        if (j == 0 && fault == 8) attrs.erase("offset");
        if (j == 0 && fault == 9) attrs.erase("readable");
        if (j == 0 && fault == 10) attrs.erase("writeable");
        rows.push_back(j == 0 && fault == 11 ? Attribute(b.getStringAttr("bad")) :
            Attribute(attrs.getDictionary(&context)));
      }
      if (!(fault == 12 && fragment == 0))
        bank->setAttr("goldengate.mmioRegisters", b.getArrayAttr(rows));
    }
  };
  auto printed = [](ModuleOp root) {
    std::string text; llvm::raw_string_ostream out(text); root.print(out); return text;
  };
  for (auto count : counts) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin();
    setup(c, count, 99);
    auto before = printed(*root);
    SmallVector<llvm::StringRef> modules = count == 1 ? SmallVector<llvm::StringRef>{"Even"} :
        SmallVector<llvm::StringRef>{"Odd", "Even"};
    goldengate::ControlMMIOWidget widget{"sentinel", 999, 8}; std::string error;
    require(succeeded(goldengate::deriveControlMMIOWidget(c, "stream", "Adapter", modules, widget, error)), error);
    require(widget.name == "stream" && widget.registerCount == count && !widget.customSize,
        "live registry count or identity differs");
    require(printed(*root) == before, "registry query mutated IR");
    // Allocation can query the complete fragments before the adapter exists.
    // Removing the adapter must not change identity/count/IR; typed validation
    // remains mandatory when the adapter is subsequently assembled.
    named(c, "Adapter").erase();
    auto withoutAdapter = printed(*root);
    goldengate::ControlMMIOWidget early{"sentinel", 999, 8};
    require(succeeded(goldengate::deriveControlMMIORegistry(c, "stream", modules, early, error)) &&
        early.name == widget.name && early.registerCount == widget.registerCount && !early.customSize &&
        printed(*root) == withoutAdapter, "early fragment allocation differs or mutated IR");
    require(failed(goldengate::deriveControlMMIOWidget(c, "stream", "Adapter", modules, early, error)) &&
        early.name == widget.name && early.registerCount == count && !early.customSize &&
        printed(*root) == withoutAdapter, "missing late adapter accepted or changed result");
    // A growing stream bank moves ahead of its peer when rounded size grows.
    // At equal sizes it stays first, preserving registration order.
    const goldengate::ControlMMIOWidget widgets[]{widget, {"peer", 3}};
    SmallVector<goldengate::ControlMMIORegion> regions;
    require(succeeded(goldengate::allocateControlMMIORegions(12, widgets, regions, error)), error);
    auto index = count <= 2 ? 1u : 0u;
    uint64_t bytes = 4; while (bytes < 4 * count) bytes *= 2;
    require(regions[index].name == "stream" && regions[index].size == bytes &&
        regions[index].start == (index ? 16 : 0), "derived growth/tie allocation differs");
  }
  for (unsigned fault = 0; fault < 25; ++fault) {
    auto root = fixture(context); auto c = *root->getOps<CircuitOp>().begin(); setup(c, 9, fault);
    SmallVector<llvm::StringRef> modules{"Odd", "Even"};
    if (fault == 20) modules.push_back("Even");
    if (fault == 21) modules[0] = "Missing";
    if (fault == 22) modules[0] = "";
    if (fault == 23) modules.clear();
    auto before = printed(*root); std::string error;
    goldengate::ControlMMIOWidget widget{"sentinel", 999, 8};
    require(failed(goldengate::deriveControlMMIOWidget(c, fault == 24 ? "" : "stream",
        "Adapter", modules, widget, error)) && !error.empty(), "bad live registry accepted");
    require(widget.name == "sentinel" && widget.registerCount == 999 && widget.customSize == 8 &&
        printed(*root) == before, "failed registry query changed output or IR");
    // Adapter faults do not affect the fragment registry; every fragment fault
    // must still fail atomically before address allocation.
    bool fragmentFault = fault < 13 || fault >= 20;
    bool ok = succeeded(goldengate::deriveControlMMIORegistry(c,
        fault == 24 ? "" : "stream", modules, widget, error));
    require(ok == !fragmentFault, "early fragment validation differs");
    require(printed(*root) == before, "early registry query mutated IR");
    if (fragmentFault) require(widget.name == "sentinel" && widget.registerCount == 999 &&
        widget.customSize == 8 && !error.empty(), "failed early query changed result");
    else require(widget.name == "stream" && widget.registerCount == 9 && !widget.customSize,
        "adapter-independent registry count differs");
  }
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
    allocation(context); registry(context);
    // Nonzero base, holes, unsorted slave indices, and the full address-space
    // endpoint exercise the generic API independently of the U250 catalog.
    const goldengate::ControlMMIORegion sparse[]{{"high", 100, 10}, {"low", 4, 4}};
    const goldengate::ControlMMIORegion full[]{{"all", 0, uint64_t(1) << 63}};
    checkMap(context, 8, sparse); checkMap(context, 63, full);
    mapping(context); rejection(context);
    llvm::outs() << "Control decoder: " << samples << " address pairs; baseline/Print/custom allocations, catalog, wiring, targets, 13 live registry/growth cases, 25 registry, 12 allocation and 15 IR atomic rejections passed\n";
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
