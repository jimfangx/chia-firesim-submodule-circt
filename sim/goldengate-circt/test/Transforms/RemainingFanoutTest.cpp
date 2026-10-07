// See LICENSE for license details.
#include "goldengate/RemainingFanout.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
using goldengate::AnnotationClasses;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
OwningOpRef<ModuleOp> fixture(MLIRContext &ctx) {
  OpBuilder b(&ctx); auto loc = b.getUnknownLoc();
  auto root = ModuleOp::create(loc); b.setInsertionPointToStart(root.getBody());
  auto circuit = b.create<CircuitOp>(loc, b.getStringAttr("Top"));
  b.setInsertionPointToStart(circuit.getBodyBlock());
  auto bit = UIntType::get(&ctx, 1);
  auto payload = BundleType::get(&ctx, {{b.getStringAttr("a"), false, bit},
                                       {b.getStringAttr("b"), false, bit}});
  SmallVector<PortInfo> ports{{b.getStringAttr("producer"), payload, Direction::Out},
      {b.getStringAttr("other"), bit, Direction::Out},
      {b.getStringAttr("input"), bit, Direction::In}};
  b.create<FModuleOp>(loc, b.getStringAttr("Top"),
      ConventionAttr::get(&ctx, Convention::Internal), ports);
  auto source = [&](StringRef field) { return b.getStringAttr("~Top|Top>" + field.str()); };
  auto a = source("producer.a"), other = source("other"), z = source("producer.b");
  SmallVector<Attribute> raw;
  auto channel = [&](StringRef name, ArrayRef<Attribute> sources, unsigned latency = 0,
                     StringRef kind = AnnotationClasses::PipeChannel, bool bridge = false) {
    SmallVector<NamedAttribute> fields{
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr(name)),
        b.getNamedAttr("channelInfo", b.getDictionaryAttr({
            b.getNamedAttr("class", b.getStringAttr(kind)),
            b.getNamedAttr("latency", b.getI64IntegerAttr(latency))}))};
    if (!bridge) fields.push_back(b.getNamedAttr("sources", b.getArrayAttr(sources)));
    if (latency) {
      fields.push_back(b.getNamedAttr("clock", source("input")));
      fields.push_back(b.getNamedAttr("sinks", b.getArrayAttr({source("input")})));
    }
    raw.push_back(b.getDictionaryAttr(fields));
  };
  // First occurrence determines group/name order; duplicate names are a set.
  channel("a0", {a}); channel("b0", {other}, 1);
  channel("a1", {a}, 1); channel("a0", {a});
  channel("ordered0", {a, z}); channel("reversed0", {z, a});
  channel("ordered1", {a, z}, 1); channel("b1", {other});
  channel("rv", {a}, 0, AnnotationClasses::DecoupledForwardChannel);
  channel("bridge0", {}, 0, AnnotationClasses::PipeChannel, true);
  channel("bridge1", {}, 0, AnnotationClasses::PipeChannel, true);
  raw.push_back(b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelFanout)),
      b.getNamedAttr("channelNames", b.getArrayAttr({b.getStringAttr("bridge1"), b.getStringAttr("bridge0")}))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(raw));
  return root;
}
} // namespace
int main() {
  try {
    MLIRContext ctx; ctx.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    auto root = fixture(ctx); auto circuit = *root->getOps<CircuitOp>().begin();
    auto before = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    auto module = *circuit.getOps<FModuleOp>().begin();
    auto moduleAttrs = module->getAttrDictionary();
    std::string error;
    require(succeeded(goldengate::addRemainingFanoutAnnotations(circuit, error)), error);
    auto after = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(after.size() == before.size() + 3, "wrong fanout count");
    for (unsigned i = 0; i < before.size(); ++i)
      require(after[i] == before[i], "changed existing annotation");
    require(module->getAttrDictionary() == moduleAttrs, "changed module attributes");
    const char *expected[] = {"a0,a1", "b0,b1", "ordered0,ordered1"};
    for (unsigned i = 0; i < 3; ++i) {
      Annotation annotation(after[before.size()+i]);
      require(annotation.isClass(AnnotationClasses::ChannelFanout), "wrong generated class");
      std::string names;
      for (auto name : annotation.getMember<ArrayAttr>("channelNames")) {
        if (!names.empty()) names += ",";
        names += cast<StringAttr>(name).getValue();
      }
      require(names == expected[i], "wrong source/name insertion order: " + names);
      llvm::outs() << "GROUP " << names << '\n';
    }
    // Scala's append-only step is deliberately one-shot, including existing groups.
    require(succeeded(goldengate::addRemainingFanoutAnnotations(circuit, error)), error);
    require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").size() == after.size()+3,
            "changed Scala repeated-execution contract");
    // Invalid post-FAME identities must fail before any group is appended.
    for (const char *bad : {"~Top|Top>missing", "~Top|Top>input", "~Other|Top>other"}) {
      auto malformed = fixture(ctx); auto c = *malformed->getOps<CircuitOp>().begin();
      OpBuilder b(&ctx); auto original = c->getAttrOfType<ArrayAttr>("rawAnnotations");
      SmallVector<Attribute> raw(original.begin(), original.end());
      NamedAttrList fields(cast<DictionaryAttr>(raw.back()));
      fields.set("class", b.getStringAttr(AnnotationClasses::ChannelConnection));
      fields.set("globalName", b.getStringAttr("bad"));
      fields.set("channelInfo", b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::PipeChannel))}));
      fields.set("sources", b.getArrayAttr({b.getStringAttr(bad)}));
      raw.push_back(fields.getDictionary(&ctx)); c->setAttr("rawAnnotations", b.getArrayAttr(raw));
      auto snapshot = c->getAttrDictionary(); error.clear();
      require(failed(goldengate::addRemainingFanoutAnnotations(c, error)) && !error.empty(), "accepted invalid source");
      require(c->getAttrDictionary() == snapshot, "failed preflight mutated annotations");
    }
    llvm::outs() << "PASS remaining model fanout: ordered identities, latencies, name deduplication, preservation, atomic rejection\n";
    return 0;
  } catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
}
