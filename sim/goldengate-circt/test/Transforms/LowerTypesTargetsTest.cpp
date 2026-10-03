// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/InnerSymbolTable.h"
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
      firrtl.module @Top(in %io: !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>,
                        in %io_a_b: !firrtl.uint<10>,
                        in %io_a_b_0: !firrtl.uint<13>) {
        %reserved = firrtl.wire : !firrtl.uint<1>
      }
      firrtl.module @Parent() {}
    }
  })mlir", &context);
  require(bool(collision), "colliding port fixture parse failed");
  auto cc = *collision->getOps<CircuitOp>().begin();
  auto cm = *cc.getOps<FModuleOp>().begin();
  using namespace circt::hw;
  auto property = [&](StringRef name, unsigned field, StringRef visibility) {
    return InnerSymPropertiesAttr::get(&context, b.getStringAttr(name), field,
                                       b.getStringAttr(visibility));
  };
  cm.setPortSymbolsAttr(0, InnerSymAttr::get(&context, {
      property("nested_id", 2, "private"), property("flat_id", 3, "public")}));
  cm.setPortSymbolsAttr(1, InnerSymAttr::get(&context, {
      property("scalar_id", 0, "public")}));
  auto reserved = *cm.getOps<WireOp>().begin();
  reserved.setInnerSymAttr(InnerSymAttr::get(b.getStringAttr("gg_lower_target")));
  auto parent = *std::next(cc.getOps<FModuleOp>().begin());
  b.setInsertionPointToEnd(parent.getBodyBlock());
  auto instance = b.create<InstanceOp>(parent.getLoc(), cm, "child");
  instance->setAttr("test.metadata", b.getStringAttr("preserve"));
  auto refs = b.getArrayAttr({
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("nested_id")),
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("flat_id")),
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("scalar_id")),
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("gg_lower_target"))});
  cc->setAttr("test.stable_refs", refs);
  cc->setAttr("rawAnnotations", b.getArrayAttr({
      annotation("~Top|Top>io", "root"),
      annotation("~Top|Top>io.a.b", "nested"),
      annotation("~Top|Top>io.a_b", "flat"),
      annotation("~Top|Top>io_a_b", "scalar"),
      annotation("~Top|Top>io.v", "vector"),
      annotation("~Top|Top>io_a_b", "scalar-event",
                 goldengate::AnnotationClasses::AutoCounter),
      annotation("~Top|Top>io.a.b", "nested-event",
                 goldengate::AnnotationClasses::AutoCounter)}));
  require(succeeded(verify(*collision)), "initial collision identities invalid");
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*collision, cc, error)) &&
              succeeded(verify(*collision)), "colliding targets failed lowering: " + error);
  auto ca = cc->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<unsigned> widths{8, 9, 11, 12, 8, 9, 10, 11, 10, 8};
  require(ca.size() == widths.size(), "colliding target expansion count changed");
  llvm::DenseMap<unsigned, StringAttr> names;
  for (auto [index, attr] : llvm::enumerate(ca)) {
    Annotation result(attr);
    auto spelling = result.getMember<StringAttr>("target");
    auto target = goldengate::resolveAnnotationTarget(cc, spelling.getValue(), error);
    require(target && target->port && *target->fieldID == 0 &&
                cast<UIntType>(target->module.getPortType(*target->port)).getWidth() == widths[index],
            "colliding annotation bound to the wrong original leaf at " +
                std::to_string(index) + ": " + spelling.getValue().str() + "\n" + dump(*collision));
    auto [it, inserted] = names.try_emplace(widths[index], spelling);
    require(inserted || it->second == spelling, "overlapping selectors lost leaf identity");
    require(result.getMember<ArrayAttr>("test.payload") ==
                Annotation(annotations.front()).getMember<ArrayAttr>("test.payload"),
            "colliding annotation payload changed");
  }
  require(names.size() == 5 && names[8] != names[9] && names[8] != names[10] &&
              names[9] != names[10] && names[11] != names[12],
          "distinct colliding leaves share an annotation target");
  auto untouched = goldengate::resolveAnnotationTarget(cc, "~Top|Top>io_a_b_0", error);
  require(untouched && untouched->port &&
              cast<UIntType>(cm.getPortType(*untouched->port)).getWidth() == 13,
          "namespace rename stole a preexisting unique port name");
  auto loweredInstance = *parent.getOps<InstanceOp>().begin();
  require(loweredInstance.getPortNamesAttr() == cm.getPortNamesAttr() &&
              loweredInstance.getNumResults() == cm.getNumPorts() &&
              loweredInstance->getAttrOfType<StringAttr>("test.metadata").getValue() == "preserve",
          "instance port names or metadata did not follow module renames");
  for (unsigned port = 0; port < cm.getNumPorts(); ++port)
    require(loweredInstance.getResult(port).getType() == cm.getPortType(port),
            "instance leaf ordering/types changed during namespace rename");
  InnerSymbolTable identities(cm);
  for (auto [name, width, visibility] : {
      std::tuple<StringRef, unsigned, StringRef>{"nested_id", 8, "private"},
      {"flat_id", 9, "public"}, {"scalar_id", 10, "public"}}) {
    auto target = identities.lookup(name);
    require(target && target.isPort() && target.getField() == 0 &&
                cast<UIntType>(cm.getPortType(target.getPort())).getWidth() == width,
            "native InnerRef lost its original port identity");
    auto symbol = cm.getPortSymbolAttr(target.getPort());
    require(symbol.size() == 1 && symbol.getProps().front().getSymVisibility().getValue() == visibility,
            "native symbol visibility changed or temporary symbol leaked");
  }
  unsigned symbolCount = 0;
  InnerSymbolTable::walkSymbols(cm, [&](StringAttr, const InnerSymTarget &) { ++symbolCount; });
  require(symbolCount == 4 && identities.lookupOp("gg_lower_target") == reserved &&
              cc->getAttr("test.stable_refs") == refs,
          "temporary symbols leaked or preexisting declaration identity changed");
  before = dump(*collision);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*collision, cc, error)) &&
              dump(*collision) == before, "collision lowering is not idempotent");
  llvm::outs() << "DontTouch aggregates expand in declaration order; fields, flips and unrelated targets preserved; three invalid selectors reject atomically; namespace collisions follow leaf identities and preserve native InnerRefs without temporary symbol leakage\n";

}
} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { run(context); }
  catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
