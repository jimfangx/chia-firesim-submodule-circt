// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void run(MLIRContext &context) {
  const char *fixture = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %io: !firrtl.bundle<a: uint<8>, nested: bundle<ready flip: uint<1>, data: vector<uint<4>, 2>>, rows: vector<bundle<x: uint<3>, y: sint<5>>, 2>>,
                        in %empty: !firrtl.bundle<>,
                        in %none: !firrtl.vector<uint<8>, 0>,
                        in %scalar: !firrtl.uint<8>) {
        %wire = firrtl.wire : !firrtl.uint<8>
        firrtl.strictconnect %wire, %scalar : !firrtl.uint<8>
      }
    }
  })mlir";
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "aggregate target fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto annotation = [&](StringRef target, StringRef tag,
                        StringRef klass = goldengate::AnnotationClasses::DontTouch) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass)),
        b.getNamedAttr("target", b.getStringAttr(target)),
        b.getNamedAttr("test.tag", b.getStringAttr(tag)),
        b.getNamedAttr("test.payload", b.getArrayAttr({b.getI32IntegerAttr(7)}))});
  };
  SmallVector<Attribute> annotations{
      annotation("~Top|Top>io", "root"),
      annotation("~Top|Top>io.nested", "bundle"),
      annotation("~Top|Top>io.nested.data", "vector"),
      annotation("~Top|Top>io.rows[1]", "element"),
      annotation("~Top|Top>io.rows.0", "dot-index"),
      annotation("~Top|Top>io.nested.data[1]", "leaf"),
      annotation("~Top|Top>empty", "empty-bundle"),
      annotation("~Top|Top>none", "empty-vector"),
      annotation("~Top|Top>scalar", "scalar"),
      annotation("~Top|Top>wire", "internal"),
      annotation("~Top|Top>io.nested.data[0]", "event",
                 goldengate::AnnotationClasses::AutoCounter),
      annotation("~Top|Top>io", "unrelated", "test.Unknown")};
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  std::string error;
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
  require(succeeded(verify(*root)), "lowered fixture verification failed");
  SmallVector<std::pair<StringRef, StringRef>> expected{
      {"root", "io_a"}, {"root", "io_nested_ready"},
      {"root", "io_nested_data_0"}, {"root", "io_nested_data_1"},
      {"root", "io_rows_0_x"}, {"root", "io_rows_0_y"},
      {"root", "io_rows_1_x"}, {"root", "io_rows_1_y"},
      {"bundle", "io_nested_ready"}, {"bundle", "io_nested_data_0"},
      {"bundle", "io_nested_data_1"},
      {"vector", "io_nested_data_0"}, {"vector", "io_nested_data_1"},
      {"element", "io_rows_1_x"}, {"element", "io_rows_1_y"},
      {"dot-index", "io_rows_0_x"}, {"dot-index", "io_rows_0_y"},
      {"leaf", "io_nested_data_1"}, {"scalar", "scalar"},
      {"internal", "wire"}, {"event", "io_nested_data_0"}};
  auto lowered = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(lowered.size() == expected.size() + 1,
          "aggregate expansion count or empty aggregate consumption changed");
  for (auto [index, item] : llvm::enumerate(expected)) {
    Annotation result(lowered[index]);
    require(result.getMember<StringAttr>("test.tag").getValue() == item.first &&
                result.getMember<StringAttr>("target").getValue() ==
                    "~Top|Top>" + item.second.str(),
            "retained target expansion/order mismatch");
    require(result.getMember<ArrayAttr>("test.payload") ==
                Annotation(annotations.front()).getMember<ArrayAttr>("test.payload"),
            "annotation payload changed during expansion");
    require(result.isClass(item.first == "event" ?
                goldengate::AnnotationClasses::AutoCounter :
                goldengate::AnnotationClasses::DontTouch),
            "annotation class changed during expansion");
    if (item.first != "internal") {
      auto target = goldengate::resolveAnnotationTarget(circuit,
          result.getMember<StringAttr>("target").getValue(), error);
      require(target && target->port && *target->fieldID == 0 &&
                  cast<FIRRTLBaseType>(target->module.getPortType(*target->port)).isGround(),
              "expanded annotation does not resolve to a ground CIRCT port");
    }
  }
  require(lowered[expected.size()] == annotations.back(),
          "unrelated annotation was modified");
  auto module = *circuit.getOps<FModuleOp>().begin();
  auto ready = goldengate::resolveAnnotationTarget(circuit, "~Top|Top>io_nested_ready", error);
  require(ready && module.getPortDirection(*ready->port) == Direction::Out,
          "nested flipped leaf direction changed");
  auto before = dump(*root);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)) &&
              dump(*root) == before, "retained target lowering is not idempotent");
  // Invalid selectors and nonscalar event targets must be rejected before
  // lowering any earlier valid target or publishing an annotation expansion.
  for (auto bad : {annotation("~Top|Top>io.rows[2]", "bad"),
                   annotation("~Top|Top>io.absent", "bad"),
                   annotation("~Top|Top>io.nested", "bad-event",
                              goldengate::AnnotationClasses::AutoCounter)}) {
    auto invalid = parseSourceString<ModuleOp>(fixture, &context);
    require(bool(invalid), "invalid fixture parse failed");
    auto ic = *invalid->getOps<CircuitOp>().begin();
    ic->setAttr("rawAnnotations", b.getArrayAttr({annotations.front(), bad}));
    before = dump(*invalid);
    require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, ic, error)) &&
                !error.empty() && dump(*invalid) == before,
            "invalid target was accepted or mutated IR");
  }
  auto collision = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %io: !firrtl.bundle<a: uint<8>>,
                        in %io_a: !firrtl.uint<8>) {}
    }
  })mlir", &context);
  require(bool(collision), "colliding port fixture parse failed");
  auto cc = *collision->getOps<CircuitOp>().begin();
  cc->setAttr("rawAnnotations", b.getArrayAttr({annotations.front()}));
  before = dump(*collision);
  require(failed(goldengate::lowerTypesWithRetainedTargets(*collision, cc, error)) &&
              error.find("namespace rename") != std::string::npos &&
              dump(*collision) == before,
          "colliding target rebound to another port or mutated IR");
  llvm::outs() << "DontTouch root/subbundle/vector/index/leaf targets expand in declaration order; empty aggregates consumed; fields, flips, ground/internal and unrelated targets preserved; four invalid plans reject without mutation\n";
}
} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { run(context); }
  catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
