// See LICENSE for license details.
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/AutoILAAnalysis.h"
#include "goldengate/LowerTypes.h"
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
      firrtl.module @Top(in %clock: !firrtl.clock,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<data: uint<8>, flag: uint<1>>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>) {}
      firrtl.module @Model(in %clock: !firrtl.clock,
                           in %data: !firrtl.uint<8>,
        in %input_sink: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: uint<8>>,
        out %output_source: !firrtl.bundle<ready flip: uint<1>, valid: uint<1>, bits: bundle<result: uint<8>>>) {
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
  model.setPortAnnotationsAttr(b.getArrayAttr({attached, attached, b.getArrayAttr({}), b.getArrayAttr({})}));
  auto wire = *model.getOps<WireOp>().begin();
  wire->setAttr("annotations", attached);
  OpBuilder body(model.getBodyBlock(), model.getBodyBlock()->end());
  auto child = body.create<InstanceOp>(model.getLoc(), other, "child");
  child.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  OpBuilder wrapper(top.getBodyBlock(), top.getBodyBlock()->end());
  auto parentInstance = wrapper.create<InstanceOp>(top.getLoc(), other, "other");
  parentInstance.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  other.setPortAnnotationsAttr(b.getArrayAttr({attached}));
  top.setPortAnnotationsAttr(b.getArrayAttr({attached, b.getArrayAttr({}), b.getArrayAttr({})}));
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
              b.getArrayAttr({b.getArrayAttr({kept}), b.getArrayAttr({kept}), b.getArrayAttr({}), b.getArrayAttr({})}) &&
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
  auto input = dt("~Top|Top>data");
  auto output = dt("~Top|Top>result");
  auto unrelated = annotation("test.Retained", "~Top|Top>data");
  auto hierarchy = dt("~Top|Top/model:Model>data");
  auto otherModule = dt("~Top|Other>data");
  circuit->setAttr("rawAnnotations", b.getArrayAttr(
      {input, output, unrelated, hierarchy, otherModule, input}));
  before = dump(*root);
  for (StringRef invalid : {"~Top|Top>input_sink", "~Top|Top>input_sink.ready",
                            "~Top|Top>input_sink.bits.missing", "~Top|Model>data",
                            "~Foreign|Top>input_sink.bits"})
    require(failed(goldengate::transferFAMEWrapperDontTouch(
                circuit, "~Top|Top>data", invalid, error)) && dump(*root) == before,
            "invalid replacement mutated retained targets");
  require(failed(goldengate::transferFAMEWrapperDontTouch(
              circuit, "~Top|Top/model:Model>data", "~Top|Top>input_sink.bits.data", error)) &&
              dump(*root) == before, "hierarchical source accepted or mutated archive");
  require(succeeded(goldengate::transferFAMEWrapperDontTouch(
              circuit, "~Top|Top>data", "~Top|Top>input_sink.bits.data", error)) &&
          succeeded(goldengate::transferFAMEWrapperDontTouch(
              circuit, "~Top|Top>result", "~Top|Top>output_source.bits", error)),
          "wrapper payload transfer failed");
  auto newInput = dt("~Top|Top>input_sink.bits.data");
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr(
              {newInput, dt("~Top|Top>output_source.bits"), unrelated,
               hierarchy, otherModule, newInput}),
          "wrong class/target transferred or duplicates/order lost");
  before = dump(*root);
  require(succeeded(goldengate::transferFAMEWrapperDontTouch(
              circuit, "~Top|Top>data", "~Top|Top>input_sink.bits.data", error)) &&
              dump(*root) == before, "wrapper transfer is not idempotent");
  auto debug = [&](StringRef target) {
    return annotation(goldengate::AnnotationClasses::InternalFpgaDebug, target);
  };
  // SFC InternalFirrtlFpgaDebugAnnotation.update(hostDecouplingRenames)
  // preserves ComponentName spelling, including the channel payload fields.
  SmallVector<Attribute> selections{
      debug("Top.Top.data"), debug("Top.Model.data"), debug("Top.Model.result"),
      debug("Top.Model.state"), debug("Top.Other.data"),
      debug("~Top|Model>data"), debug("Top.Model.data"), unrelated, hierarchy};
  circuit->setAttr("rawAnnotations", b.getArrayAttr(selections));
  before = dump(*root);
  for (StringRef invalid : {"~Top|Model>input_sink.valid", "~Top|Model>input_sink",
                            "~Top|Model>output_source.bits.missing",
                            "~Top|Top>input_sink.bits.data"})
    require(failed(goldengate::transferFAMEPortDebugTargets(
                circuit, "~Top|Model>data", invalid, error)) && dump(*root) == before,
            "invalid debug payload mutated archive");
  require(failed(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Top/model:Model>data", "~Top|Model>input_sink.bits", error)) &&
              dump(*root) == before, "hierarchical debug source accepted");
  selections.push_back(b.getDictionaryAttr({b.getNamedAttr("class",
      b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(selections));
  before = dump(*root);
  require(failed(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Model>data", "~Top|Model>input_sink.bits", error)) &&
              dump(*root) == before, "malformed debug annotation partially transferred");
  selections.pop_back();
  circuit->setAttr("rawAnnotations", b.getArrayAttr(selections));
  require(succeeded(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Top>data", "~Top|Top>input_sink.bits.data", error)) &&
          succeeded(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Model>data", "~Top|Model>input_sink.bits", error)) &&
          succeeded(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Model>result", "~Top|Model>output_source.bits.result", error)),
          "FPGA debug payload transfer failed");
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr({
      debug("Top.Top.input_sink.bits.data"), debug("Top.Model.input_sink.bits"),
      debug("Top.Model.output_source.bits.result"), debug("Top.Model.state"),
      debug("Top.Other.data"), debug("~Top|Model>input_sink.bits"),
      debug("Top.Model.input_sink.bits"), unrelated, hierarchy}),
      "SFC debug target rename mismatch or archive order/duplicates changed");
  before = dump(*root);
  require(succeeded(goldengate::transferFAMEPortDebugTargets(
              circuit, "~Top|Model>data", "~Top|Model>input_sink.bits", error)) &&
              dump(*root) == before, "debug payload transfer is not idempotent");
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue().take_front(5))
    llvm::outs() << "debug-target " << cast<DictionaryAttr>(attr).getAs<StringAttr>("target").getValue() << '\n';
  auto transferred = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> lateAnnotations(transferred.getValue().take_front(7));
  auto staleChannel = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
      b.getNamedAttr("ports", b.getArrayAttr({b.getStringAttr("~Top|Model>removed_input")}))});
  auto host = annotation(goldengate::AnnotationClasses::HostClockSource, "~Top|Top>clock");
  lateAnnotations.append({staleChannel, host, host, unrelated});
  circuit->setAttr("rawAnnotations", b.getArrayAttr(lateAnnotations));
  wrapper.setInsertionPointToEnd(top.getBodyBlock());
  wrapper.create<InstanceOp>(top.getLoc(), model, "model");
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(
              *root, circuit, error, goldengate::RetainedTargetScope::FpgaDebugOnly)) &&
              succeeded(verify(*root)), "channel debug target LowerTypes failed");
  require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue().take_back(4) ==
              ArrayRef<Attribute>(lateAnnotations).take_back(4),
          "late debug lowering changed historical channels or other annotation duplicates/order");
  SmallVector<goldengate::AutoILAProbe> probes;
  require(succeeded(goldengate::analyzeAutoILAProbes(circuit, probes, error)) &&
              probes.size() == 6, "lowered channel payload debug selections did not resolve");
  unsigned inputProbes = 0, outputProbes = 0;
  for (const auto &probe : probes) {
    require(probe.width == 8, "debug payload width changed during LowerTypes");
    inputProbes += probe.leafName == "input_sink_bits" ||
                   probe.leafName == "input_sink_bits_data";
    outputProbes += probe.leafName == "output_source_bits_result";
  }
  require(inputProbes == 2 && outputProbes == 1,
          "debug selections resolved to channel handshakes instead of payloads");
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
