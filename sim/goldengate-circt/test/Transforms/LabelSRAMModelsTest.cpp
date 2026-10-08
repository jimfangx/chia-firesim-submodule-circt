// See LICENSE for license details.
#include "goldengate/LabelSRAMModels.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text); op->print(out); return text;
}
void run(MLIRContext &context) {
  const char *fixture = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @ram(in %clk: !firrtl.clock) {}
      firrtl.module @Top(in %clock: !firrtl.clock, in %other: !firrtl.clock,
                         in %addr: !firrtl.uint<2>, in %en: !firrtl.uint<1>,
                         in %data: !firrtl.uint<8>, out %out: !firrtl.uint<8>) {
        %r, %rw, %w = firrtl.mem interesting_name Undefined
          {depth = 4 : i64, name = "ram", portNames = ["r", "rw", "w"],
           readLatency = 1 : i32, writeLatency = 1 : i32} :
          !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data flip: uint<8>>,
          !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, rdata flip: uint<8>, wmode: uint<1>, wdata: uint<8>, wmask: uint<1>>,
          !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data: uint<8>, mask: uint<1>>
        %rclk = firrtl.subfield %r[clk] : !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data flip: uint<8>>
        %wclk = firrtl.subfield %w[clk] : !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data: uint<8>, mask: uint<1>>
        %rdata = firrtl.subfield %r[data] : !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data flip: uint<8>>
        %wdata = firrtl.subfield %w[data] : !firrtl.bundle<addr: uint<2>, en: uint<1>, clk: clock, data: uint<8>, mask: uint<1>>
        firrtl.connect %rclk, %clock : !firrtl.clock, !firrtl.clock
        firrtl.connect %wclk, %other : !firrtl.clock, !firrtl.clock
        firrtl.connect %out, %rdata : !firrtl.uint<8>, !firrtl.uint<8>
        firrtl.connect %wdata, %data : !firrtl.uint<8>, !firrtl.uint<8>
      }
    }
  })mlir";
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "parse SRAM fixture");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto label = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::MemModel)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top>ram"))});
  auto unrelated = b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("test.Other"))});
  circuit->setAttr("rawAnnotations", b.getArrayAttr({label, unrelated, label}));
  MemOp original;
  root->walk([&](MemOp op) { original = op; });
  auto originalAttributes = original->getAttrDictionary();
  unsigned extracted = 0;
  std::string error;
  require(succeeded(goldengate::labelSRAMModels(circuit, extracted, error)), error);
  require(extracted == 1 && succeeded(verify(*root)), "one verified wrapper per memory identity");
  auto wrapper = cast<FModuleOp>(circuit.getBodyBlock()->front());
  require(wrapper.getName() == "ram_0", "circuit namespace collision");
  require(wrapper.getNumPorts() == 4 && wrapper.getPortName(0) == "clk" &&
      wrapper.getPortName(1) == "r" && wrapper.getPortName(2) == "w" &&
      wrapper.getPortName(3) == "rw", "read/write/readwrite wrapper order");
  MemOp clone;
  wrapper.walk([&](MemOp op) { clone = op; });
  require(clone && clone->getAttrDictionary() == originalAttributes,
          "memory depth, latency, RUW, and metadata preserved");
  unsigned clockWires = 0, outputWires = 0;
  wrapper.walk([&](ConnectOp op) {
    if (op.getSrc() == wrapper.getArgument(0)) ++clockWires;
    if (auto dest = dyn_cast<SubfieldOp>(op.getDest().getDefiningOp()))
      if (isa<BlockArgument>(dest.getInput())) ++outputWires;
  });
  require(clockWires == 3 && outputWires == 2, "clocks and flipped read data connected");
  FModuleOp parent;
  for (auto mod : circuit.getOps<FModuleOp>()) if (mod.getName() == "Top") parent = mod;
  auto instance = *parent.getOps<InstanceOp>().begin();
  require(instance.getName() == "ram" && instance.getModuleName() == "ram_0", "original instance identity");
  SmallVector<Value> clocks;
  parent.walk([&](ConnectOp op) {
    if (op.getDest() == instance.getResult(0)) clocks.push_back(op.getSrc());
  });
  require(clocks.size() == 2 && clocks[0] == parent.getArgument(0) &&
      clocks[1] == parent.getArgument(1), "Scala last-connect clock order preserved");
  auto annos = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(annos.size() == 6 && annos[0] == unrelated, "memory labels consumed, other annotation retained");
  require(cast<DictionaryAttr>(annos[1]).getAs<StringAttr>("target").getValue() == "~Top|Top/ram:ram_0", "FAME instance target");
  require(cast<DictionaryAttr>(annos[3]).getAs<StringAttr>("data").getValue() == "~Top|ram_0>r.data" &&
      cast<DictionaryAttr>(annos[4]).getAs<StringAttr>("mask").getValue() == "~Top|ram_0>w.mask" &&
      cast<DictionaryAttr>(annos[5]).getAs<StringAttr>("wmode").getValue() == "~Top|ram_0>rw.wmode", "typed model port annotations");
  std::string after = dump(*root);
  require(succeeded(goldengate::labelSRAMModels(circuit, extracted, error)) &&
      extracted == 0 && after == dump(*root), "idempotent after consuming labels");

  // Extraction must run before aggregate memory data is split by LowerTypes.
  std::string aggregateFixture(fixture);
  for (auto [from, to] : {std::pair<std::string, std::string>{"uint<8>", "bundle<only: uint<8>>"},
                         {"mask: uint<1>", "mask: bundle<only: uint<1>>"}}) {
    for (size_t offset = 0; (offset = aggregateFixture.find(from, offset)) != std::string::npos;) {
      aggregateFixture.replace(offset, from.size(), to);
      offset += to.size();
    }
  }
  auto aggregate = parseSourceString<ModuleOp>(aggregateFixture, &context);
  require(bool(aggregate), "parse aggregate-data SRAM fixture");
  auto aggregateCircuit = *aggregate->getOps<CircuitOp>().begin();
  aggregateCircuit->setAttr("rawAnnotations", b.getArrayAttr({label}));
  require(succeeded(goldengate::labelSRAMModels(aggregateCircuit, extracted, error)) &&
      extracted == 1 && succeeded(verify(*aggregate)), "aggregate data and masks survive extraction");

  auto symbolic = parseSourceString<ModuleOp>(fixture, &context);
  auto symbolicCircuit = *symbolic->getOps<CircuitOp>().begin();
  symbolicCircuit->setAttr("rawAnnotations", b.getArrayAttr({label}));
  symbolic->walk([&](MemOp op) {
    op.setInnerSymAttr(circt::hw::InnerSymAttr::get(b.getStringAttr("protected_memory")));
  });
  auto symbolicBefore = dump(*symbolic);
  require(failed(goldengate::labelSRAMModels(symbolicCircuit, extracted, error)) &&
      extracted == 0 && symbolicBefore == dump(*symbolic), "inner-symbol rejection is atomic");

  // Whole-bundle users cannot be retargeted without an aggregate expansion.
  // A rejected input must not partially extract a selected memory.
  auto invalid = parseSourceString<ModuleOp>(fixture, &context);
  auto invalidCircuit = *invalid->getOps<CircuitOp>().begin();
  invalidCircuit->setAttr("rawAnnotations", b.getArrayAttr({label}));
  MemOp invalidMem;
  invalid->walk([&](MemOp op) { invalidMem = op; });
  b.setInsertionPointAfter(invalidMem);
  auto wire = b.create<WireOp>(invalidMem.getLoc(), invalidMem.getResult(0).getType(), "aggregate");
  b.create<ConnectOp>(invalidMem.getLoc(), wire.getResult(), invalidMem.getResult(0));
  auto before = dump(*invalid);
  require(failed(goldengate::labelSRAMModels(invalidCircuit, extracted, error)) &&
      extracted == 0 && before == dump(*invalid), "aggregate rejection is atomic");
}
} // namespace
int main() {
  DialectRegistry registry;
  registry.insert<FIRRTLDialect, circt::hw::HWDialect>();
  MLIRContext context(registry);
  try { run(context); } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n'; return 1;
  }
  llvm::outs() << "PASS CIRCT SRAM extraction identity, wiring, metadata, and atomicity\n";
  return 0;
}
