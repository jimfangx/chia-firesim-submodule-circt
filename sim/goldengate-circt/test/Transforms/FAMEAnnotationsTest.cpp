// See LICENSE for license details.
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <iterator>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(
    module { firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock) {}
      firrtl.module @Model(in %clock: !firrtl.clock,
                           in %data: !firrtl.uint<8>) {
        %state = firrtl.wire {name = "state"} : !firrtl.uint<8>
        firrtl.strictconnect %state, %data : !firrtl.uint<8>
      }
      firrtl.module @Other(in %data: !firrtl.uint<8>) {}
    } }
  )mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  auto model = *std::next(circuit.getOps<FModuleOp>().begin());
  auto other = *std::next(circuit.getOps<FModuleOp>().begin(), 2);
  Builder b(&context);
  auto annotation = [&](StringRef cls, StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls)),
                               b.getNamedAttr("target", b.getStringAttr(target))});
  };
  auto dt = [&](StringRef target) {
    return annotation(goldengate::AnnotationClasses::DontTouch, target);
  };
  auto kept = annotation("test.Retained", "~Top|Model>data");
  SmallVector<Attribute> all{dt("~Top|Model>clock"), dt("~Top|Model>data.field[0]"),
                           dt("~Top|Model>state"), dt("~Top|Model/child:Other>data"),
                           dt("~Top|Top/model:Model>data"), dt("~Top|Other>data"),
                           dt("~Foreign|Model>data"), kept};
  auto raw = b.getArrayAttr(all);
  circuit->setAttr("rawAnnotations", raw);
  auto attached = b.getArrayAttr({dt("~Top|Model>data"), kept});
  model.setPortAnnotationsAttr(b.getArrayAttr({attached, attached}));
  auto wire = *model.getOps<WireOp>().begin();
  wire->setAttr("annotations", attached);
  OpBuilder body(model.getBodyBlock(), model.getBodyBlock()->end());
  auto child = body.create<InstanceOp>(model.getLoc(), other, "child");
  child.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  OpBuilder wrapper(top.getBodyBlock(), top.getBodyBlock()->end());
  auto parentInstance = wrapper.create<InstanceOp>(top.getLoc(), other, "other");
  parentInstance.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  other.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  top.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  model.setPortSymbolsAttr(1, circt::hw::InnerSymAttr::get(b.getStringAttr("data_id")));
  std::string before = dump(*root), error;
  require(failed(goldengate::consumeFAMEModelDontTouches(circuit, {top}, error)) &&
              dump(*root) == before, "top selection mutated annotations");
  require(failed(goldengate::consumeFAMEModelDontTouches(circuit, {model, model}, error)) &&
              dump(*root) == before, "duplicate selection mutated annotations");
  for (StringRef invalid : {"Top|Model>data", "~Top|Model", "~Top|Model>data>field"}) {
    all.push_back(dt(invalid));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(all));
    before = dump(*root);
    require(failed(goldengate::consumeFAMEModelDontTouches(circuit, {model}, error)) &&
                dump(*root) == before, "invalid target changed archive or attached IR");
    all.pop_back();
  }
  circuit->setAttr("rawAnnotations", raw);
  require(succeeded(goldengate::consumeFAMEModelDontTouches(circuit, {model}, error)) &&
              succeeded(verify(*root)), "DontTouch consumption failed verification");
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations") ==
              b.getArrayAttr(ArrayRef<Attribute>(all).drop_front(4)),
          "wrong module identity selected or annotation order changed");
  require(model.getPortAnnotationsAttr() ==
              b.getArrayAttr({b.getArrayAttr({kept}), b.getArrayAttr({kept})}) &&
              wire->getAttrOfType<ArrayAttr>("annotations") == b.getArrayAttr({kept}) &&
              child.getPortAnnotationsAttr()[0] == b.getArrayAttr({kept}),
          "model port/body DontTouches remain or another class was lost");
  require(top.getPortAnnotationsAttr()[0] == attached &&
              parentInstance.getPortAnnotationsAttr()[0] == attached &&
              other.getPortAnnotationsAttr()[0] == attached &&
              cast<circt::hw::InnerSymAttr>(model.getPortSymbolsAttr()[1]).getSymName() == "data_id",
          "wrapper/non-model annotations or identity symbols changed");
  before = dump(*root);
  require(succeeded(goldengate::consumeFAMEModelDontTouches(circuit, {model}, error)) &&
              dump(*root) == before, "DontTouch consumption is not idempotent");
  llvm::outs() << "Consumed transformed-model reference protections; retained wrapper, "
                  "other-module, foreign-circuit and other-class annotations; atomic rejects pass\n";
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context);
    return 0;
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
}
