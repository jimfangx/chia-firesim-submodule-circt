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
#include <set>
#include <map>
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

  // The enabled production platform retains the selector while promoting its
  // corresponding model. SFC fans both identities through every parent use.
  auto threaded = parseSourceString<ModuleOp>(fixture, &context);
  auto threadedCircuit = *threaded->getOps<CircuitOp>().begin();
  auto selector = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Parent/ram:Ram"))});
  threadedCircuit->setAttr("rawAnnotations", b.getArrayAttr({model, selector, port}));
  error.clear();
  require(succeeded(goldengate::extractModels(threadedCircuit, promoted, error)) &&
          promoted == 3 && succeeded(verify(*threaded)), "threading selector promotion: " + error);
  std::set<std::string> peerTargets, selectedTargets;
  threadedCircuit.walk([&](InstanceOp instance) {
    if (instance.getModuleName() == "Ram" &&
        instance->getParentOfType<FModuleOp>().getName() == "Top")
      peerTargets.insert("~Top|Top/" + instance.getName().str() + ":Ram");
  });
  for (Attribute attr : threadedCircuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::EnableModelMultiThreading))
      selectedTargets.insert(annotation.getMember<StringAttr>("target").getValue().str());
    else
      require(attr == port, "unexpected retained annotation after selector promotion");
  }
  require(peerTargets.size() == 4 && peerTargets == selectedTargets,
          "threading selector lost one-to-many promoted instance identity");
  auto malformed = parseSourceString<ModuleOp>(fixture, &context);
  auto malformedCircuit = *malformed->getOps<CircuitOp>().begin();
  auto fieldSelector = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::EnableModelMultiThreading)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Parent/ram:Ram>r.data"))});
  malformedCircuit->setAttr("rawAnnotations", b.getArrayAttr({model, fieldSelector}));
  auto malformedBefore = dump(*malformed);
  require(failed(goldengate::extractModels(malformedCircuit, promoted, error)) &&
          promoted == 0 && dump(*malformed) == malformedBefore,
          "field-qualified threading selection was accepted or mutated IR");

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
  auto reference = [&](StringRef target, StringRef klass = goldengate::AnnotationClasses::DontTouch) {
    return b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(klass)),
        b.getNamedAttr("target", b.getStringAttr(target)),
        b.getNamedAttr("description", b.getStringAttr("~Top|Parent/ram:Ram>r.data")),
        b.getNamedAttr("constructor", b.getDictionaryAttr({
          b.getNamedAttr("target", b.getStringAttr("~Top|Parent/ram:Ram>r.data"))}))});
  };
  auto external = reference("~Top|Parent/ram:Ram>r.data", "test.InstanceReference");
  auto nested = reference("~Top|Top/m0:Middle/p0:Parent/ram:Ram>r.addr");
  auto localPort = reference("~Top|Parent>ram.r.addr");
  auto otherCircuit = reference("~Other|Parent/ram:Ram>r.data");
  auto literal = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr("test.Note")),
      b.getNamedAttr("description", b.getStringAttr("~Top|Parent/ram:Ram>r.data"))});
  referencedCircuit->setAttr("rawAnnotations", b.getArrayAttr({model, port,
      external, external, nested, localPort, otherCircuit, literal}));
  error.clear();
  require(succeeded(goldengate::extractModels(referencedCircuit, promoted, error)) &&
          promoted == 3 && succeeded(verify(*referenced)),
          "retained hierarchical target promotion failed: " + error);
  auto targets = referencedCircuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(targets.size() == 13 && targets[0] == port &&
          targets[10] == localPort && targets[11] == otherCircuit && targets[12] == literal,
          "promotion lost duplicate annotation records, order or unrelated targets/payloads");
  std::map<std::string, unsigned> counts;
  for (unsigned i = 1; i <= 8; ++i) {
    Annotation actual(targets[i]);
    require(actual.isClass("test.InstanceReference") &&
            actual.getMember<StringAttr>("description") == Annotation(external).getMember<StringAttr>("description") &&
            actual.getMember<DictionaryAttr>("constructor") == Annotation(external).getMember<DictionaryAttr>("constructor"),
            "promotion changed opaque payloads while moving real target fields");
    ++counts[actual.getMember<StringAttr>("target").getValue().str()];
  }
  for (const auto &peer : peerTargets)
    require(counts[peer + ">r.data"] == 2, "duplicate SingleTarget records did not follow every peer");
  require(Annotation(targets[9]).getMember<StringAttr>("target").getValue() ==
              "~Top|Top/m0_p0_ram:Ram>r.addr",
          "explicit outer instance context was incorrectly fanned out or retained");

  // Native inner references and attached native annotations require their own
  // relocation policy. Reject before erasing the selected instance identity.
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto native = parseSourceString<ModuleOp>(fixture, &context);
    auto owner = *native->getOps<CircuitOp>().begin();
    owner->setAttr("rawAnnotations", b.getArrayAttr({model}));
    native->walk([&](InstanceOp instance) {
      if (instance.getModuleName() != "Ram") return;
      if (kind == 1)
        instance.setInnerSymAttr(circt::hw::InnerSymAttr::get(b.getStringAttr("native_identity")));
      else {
        auto annotations = b.getArrayAttr({b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr("test.NativeIdentity"))})});
        if (kind == 0) instance->setAttr("annotations", annotations);
        else {
          SmallVector<Attribute> ports(instance.getNumResults(), b.getArrayAttr({}));
          ports[0] = annotations;
          instance.setPortAnnotationsAttr(b.getArrayAttr(ports));
        }
      }
    });
    before = dump(*native);
    require(failed(goldengate::extractModels(owner, promoted, error)) &&
            promoted == 0 && before == dump(*native),
            "native instance identity was erased or mutated without a relocation policy");
  }

  // SFC reserves grandparent module ports as well as body declarations. The
  // first peer must avoid leaf_ram, so transitive promotion can add its bundle.
  auto collision = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Ram(in %data: !firrtl.uint<8>) {}
      firrtl.module @Leaf() {
        %data = firrtl.instance ram @Ram(in data: !firrtl.uint<8>)
      }
      firrtl.module @Parent(in %leaf_ram: !firrtl.uint<1>) {
        firrtl.instance leaf @Leaf()
      }
      firrtl.module @Top() {
        %leaf_ram = firrtl.instance p @Parent(in leaf_ram: !firrtl.uint<1>)
      }
    }
  })mlir", &context);
  require(bool(collision), "port collision fixture parse failed");
  auto collisionCircuit = *collision->getOps<CircuitOp>().begin();
  auto collisionModel = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::FAMEModel)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Leaf/ram:Ram"))});
  collisionCircuit->setAttr("rawAnnotations", b.getArrayAttr({collisionModel,
      reference("~Top|Leaf/ram:Ram>data")}));
  error.clear();
  require(succeeded(goldengate::extractModels(collisionCircuit, promoted, error)) &&
          promoted == 2 && succeeded(verify(*collision)),
          "grandparent port namespace collision rejected: " + error);
  unsigned finalPeers = 0;
  collision->walk([&](InstanceOp instance) {
    if (instance.getModuleName() != "Ram") return;
    ++finalPeers;
    require(instance.getName() == "p_leaf_ram_0", "peer name did not reserve grandparent ports like SFC");
  });
  require(finalPeers == 1 && Annotation(collisionCircuit->getAttrOfType<ArrayAttr>("rawAnnotations")[0])
              .getMember<StringAttr>("target").getValue() == "~Top|Top/p_leaf_ram_0:Ram>data",
          "collision-aware peer and annotation target identities differ");
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
