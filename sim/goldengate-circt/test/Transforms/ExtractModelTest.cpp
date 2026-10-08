// See LICENSE for license details.
#include "goldengate/ExtractModel.h"
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
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text); op->print(out); return text;
}
// A read port has both sink controls and source data. Two uses at each level
// require one-to-many promotion through every hierarchy edge, not one hop.
const char *fixture = R"mlir(module {
  firrtl.circuit "Top" attributes {rawAnnotations = []} {
    firrtl.module @Ram(in %clk: !firrtl.clock,
      in %r: !firrtl.bundle<addr: uint<2>, en: uint<1>, data flip: uint<8>>,
      in %w: !firrtl.bundle<addr: uint<2>, en: uint<1>, data: uint<8>, mask: uint<1>>) {}
    firrtl.module @Parent() {
      %clk, %r, %w = firrtl.instance ram @Ram(in clk: !firrtl.clock,
        in r: !firrtl.bundle<addr: uint<2>, en: uint<1>, data flip: uint<8>>,
        in w: !firrtl.bundle<addr: uint<2>, en: uint<1>, data: uint<8>, mask: uint<1>>)
    }
    firrtl.module @Middle() {
      firrtl.instance p0 @Parent()
      firrtl.instance p1 @Parent()
    }
    firrtl.module @Top() {
      firrtl.instance m0 @Middle()
      firrtl.instance m1 @Middle()
      %reserved = firrtl.wire : !firrtl.uint<1>
    }
  }
})mlir";
void run(MLIRContext &context) {
  OpBuilder b(&context);
  auto model = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::FAMEModel)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Parent/ram:Ram"))});
  auto port = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ModelReadPort)),
      b.getNamedAttr("data", b.getStringAttr("~Top|Ram>r.data")),
      b.getNamedAttr("addr", b.getStringAttr("~Top|Ram>r.addr")),
      b.getNamedAttr("en", b.getStringAttr("~Top|Ram>r.en"))});
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "parse model fixture");
  auto circuit = *root->getOps<CircuitOp>().begin();
  circuit->setAttr("rawAnnotations", b.getArrayAttr({model, port, model}));
  FModuleOp child;
  root->walk([&](FModuleOp op) { if (op.getName() == "Ram") child = op; });
  std::string childBefore = dump(child);
  unsigned promoted;
  std::string error;
  require(succeeded(goldengate::extractModels(circuit, promoted, error)), error);
  require(promoted == 3, "promote one Parent instance and two Middle instances");
  require(succeeded(verify(*root)), "verify flipped aggregate promotion");
  require(dump(child) == childBefore, "model body and interface preserved");
  unsigned topModels = 0, nestedModels = 0, readData = 0, clocks = 0;
  root->walk([&](InstanceOp op) {
    if (op.getModuleName() == "Ram") {
      if (op->getParentOfType<FModuleOp>().getName() == "Top") ++topModels;
      else ++nestedModels;
    }
  });
  // Expand connections down to ground leaves and check read data flows from
  // each peer while clocks flow into it. This also catches inverted flip logic.
  root->walk([&](StrictConnectOp op) {
    if (auto src = op.getSrc().getDefiningOp<SubfieldOp>())
      if (src.getFieldName() == "data")
        if (auto arg = dyn_cast<OpResult>(src.getInput()))
          if (auto inst = dyn_cast<InstanceOp>(arg.getOwner()))
            if (inst.getModuleName() == "Ram" && arg.getResultNumber() == 1)
              ++readData;
    if (auto dest = dyn_cast<OpResult>(op.getDest()))
      if (auto inst = dyn_cast<InstanceOp>(dest.getOwner()))
        if (inst.getModuleName() == "Ram" && dest.getResultNumber() == 0)
          ++clocks;
  });
  require(topModels == 4 && nestedModels == 0, "fan out all branches to circuit main");
  require(readData == 4 && clocks == 4, "read-data and clock flow at every peer");
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr({port}),
          "consume duplicate model labels, retain SRAM port identities");
  auto after = dump(*root);
  require(succeeded(goldengate::extractModels(circuit, promoted, error)) &&
          promoted == 0 && dump(*root) == after, "idempotent extraction");
  // Scala leaves already-top models in place and removes their labels.
  auto topLabel = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::FAMEModel)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top/m0_p0_ram:Ram"))});
  circuit->setAttr("rawAnnotations", b.getArrayAttr({topLabel, port}));
  require(succeeded(goldengate::extractModels(circuit, promoted, error)) &&
          promoted == 0 && dump(*root) == after, "already-top models are complete");

  // An analog leaf is unsupported even when it is nested in a bundle.
  // Detect it before removing the selected instance or adding parent ports.
  std::string analogFixture(fixture);
  for (size_t offset = 0;
       (offset = analogFixture.find("data: uint<8>", offset)) != std::string::npos;) {
    analogFixture.replace(offset, 13, "data: analog<8>");
    offset += 15;
  }
  auto analog = parseSourceString<ModuleOp>(analogFixture, &context);
  require(bool(analog), "parse aggregate analog fixture");
  auto analogCircuit = *analog->getOps<CircuitOp>().begin();
  analogCircuit->setAttr("rawAnnotations", b.getArrayAttr({model}));
  auto before = dump(*analog);
  require(failed(goldengate::extractModels(analogCircuit, promoted, error)) &&
          promoted == 0 && before == dump(*analog), "nested analog rejection before mutation");

  auto referenced = parseSourceString<ModuleOp>(fixture, &context);
  auto referencedCircuit = *referenced->getOps<CircuitOp>().begin();
  auto external = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.InstanceReference")),
      b.getNamedAttr("target", b.getStringAttr("~Top|Parent/ram:Ram>r.data"))});
  referencedCircuit->setAttr("rawAnnotations", b.getArrayAttr({model, external}));
  before = dump(*referenced);
  require(failed(goldengate::extractModels(referencedCircuit, promoted, error)) &&
          promoted == 0 && before == dump(*referenced),
          "unported external instance annotation rename rejected before mutation");
}
} // namespace
int main() {
  DialectRegistry registry;
  registry.insert<FIRRTLDialect, circt::hw::HWDialect>();
  MLIRContext context(registry);
  try { run(context); } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n'; return 1;
  }
  llvm::outs() << "PASS transitive aggregate SRAM model promotion\n";
}
