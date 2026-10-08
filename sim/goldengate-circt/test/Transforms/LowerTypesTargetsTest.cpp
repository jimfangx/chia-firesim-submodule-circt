// See LICENSE for license details.
#include "goldengate/AnnotationClasses.h"
#include "goldengate/AutoCounterAnalysis.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/HW/InnerSymbolTable.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"
#include <system_error>
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
  // Internal declarations use the same field IDs and one-to-many renames as
  // ports. Collisions span the port/declaration namespace, not SSA spellings.
  const char *internalFixture = R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>,
          in %data: !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>,
          in %agg_v_0: !firrtl.uint<15>) {
        %agg = firrtl.wire : !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>
        %agg_a_b = firrtl.wire : !firrtl.uint<10>
        %agg_a_b_0 = firrtl.wire : !firrtl.uint<13>
        %alias = firrtl.node %data : !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>
        %state = firrtl.reg %clock : !firrtl.clock, !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>
        %resetState = firrtl.regreset %clock, %reset, %data : !firrtl.clock, !firrtl.uint<1>, !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>, !firrtl.bundle<a: bundle<b: uint<8>>, a_b: uint<9>, v: vector<uint<11>, 1>, v_0: uint<12>>
        %empty = firrtl.wire : !firrtl.bundle<>
      }
    }
  })mlir";
  auto internal = parseSourceString<ModuleOp>(internalFixture, &context);
  require(bool(internal), "internal aggregate fixture parse failed");
  auto ic = *internal->getOps<CircuitOp>().begin();
  auto im = *ic.getOps<FModuleOp>().begin();
  auto aggregate = *im.getOps<WireOp>().begin();
  aggregate.setInnerSymAttr(InnerSymAttr::get(&context, {
      property("wire_leaf", 2, "public")}));
  auto state = *im.getOps<RegOp>().begin();
  state.setInnerSymAttr(InnerSymAttr::get(&context, {
      property("state_leaf", 3, "private")}));
  auto internalRefs = b.getArrayAttr({
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("wire_leaf")),
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("state_leaf"))});
  ic->setAttr("test.stable_refs", internalRefs);
  SmallVector<Attribute> internalAnnotations{
      annotation("~Top|Top>agg", "wire"),
      annotation("~Top|Top>agg.a.b", "wire-leaf"),
      annotation("~Top|Top>agg.v.0", "wire-vector"),
      annotation("~Top|Top>agg_a_b", "scalar-wire"),
      annotation("~Top|Top>alias", "node"),
      annotation("~Top|Top>state", "reg"),
      annotation("~Top|Top>resetState", "reset-reg"),
      annotation("~Top|Top>empty", "empty-wire"),
      annotation("~Top|Top>agg.v[0]", "event", goldengate::AnnotationClasses::AutoCounter)};
  ic->setAttr("rawAnnotations", b.getArrayAttr(internalAnnotations));
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*internal, ic, error)) &&
              succeeded(verify(*internal)), "internal aggregate lowering failed: " + error);
  auto ia = ic->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<unsigned> internalWidths{8, 9, 11, 12, 8, 11, 10,
                                      8, 9, 11, 12, 8, 9, 11, 12,
                                      8, 9, 11, 12, 11};
  require(ia.size() == internalWidths.size(), "internal expansion count changed");
  llvm::DenseSet<StringAttr> wireNames;
  for (auto [index, attr] : llvm::enumerate(ia)) {
    Annotation result(attr);
    auto spelling = result.getMember<StringAttr>("target");
    auto target = goldengate::resolveInternalFieldTarget(ic, spelling.getValue(), error);
    require(target && target->fieldID == 0 &&
                cast<UIntType>(target->type).getWidth() == internalWidths[index],
            "internal target lost leaf identity: " + spelling.getValue().str());
    require(result.getMember<ArrayAttr>("test.payload") ==
                Annotation(internalAnnotations.front()).getMember<ArrayAttr>("test.payload"),
            "internal annotation payload changed");
    if (index < 4 || index == 6)
      require(wireNames.insert(spelling).second, "colliding internal leaves share a name");
    require(index < 7 || index == 19 ? isa<WireOp>(target->declaration) :
            index < 11 ? isa<NodeOp>(target->declaration) :
            index < 15 ? isa<RegOp>(target->declaration) :
                         isa<RegResetOp>(target->declaration),
            "lowering changed internal declaration kind");
  }
  require(Annotation(ia[0]).getMember<StringAttr>("target") ==
              Annotation(ia[4]).getMember<StringAttr>("target") &&
              Annotation(ia[2]).getMember<StringAttr>("target") ==
              Annotation(ia[5]).getMember<StringAttr>("target") &&
              Annotation(ia[5]).getMember<StringAttr>("target") ==
              Annotation(ia[19]).getMember<StringAttr>("target"),
          "overlapping internal selectors disagree");
  InnerSymbolTable internalSymbols(im);
  for (auto [name, width, visibility] : {
      std::tuple<StringRef, unsigned, StringRef>{"wire_leaf", 8, "public"},
      {"state_leaf", 9, "private"}}) {
    auto target = internalSymbols.lookup(name);
    require(target && !target.isPort() && target.getField() == 0 &&
                cast<UIntType>(target.getOp()->getResult(0).getType()).getWidth() == width,
            "native internal leaf identity changed");
    auto symbols = cast<InnerSymbolOpInterface>(target.getOp()).getInnerSymAttr();
    require(symbols.size() == 1 &&
                symbols.getProps().front().getSymVisibility().getValue() == visibility,
            "native internal symbol visibility changed");
  }
  symbolCount = 0;
  InnerSymbolTable::walkSymbols(im, [&](StringAttr, const InnerSymTarget &) { ++symbolCount; });
  require(symbolCount == 2 && ic->getAttr("test.stable_refs") == internalRefs,
          "temporary internal symbols leaked or native references changed");
  before = dump(*internal);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*internal, ic, error)) &&
              dump(*internal) == before, "internal lowering is not idempotent");
  // Host signals and debug probes use SingleTargetAnnotation fanout.
  // Preserve distinct payloads on overlapping selectors and internal namespaces.
  for (StringRef klass : {goldengate::AnnotationClasses::HostClock,
                         goldengate::AnnotationClasses::HostReset,
                         goldengate::AnnotationClasses::HostClockSource,
                         goldengate::AnnotationClasses::HostClockSink,
                         goldengate::AnnotationClasses::FpgaDebug,
                         goldengate::AnnotationClasses::InternalFpgaDebug}) {
    auto candidate = parseSourceString<ModuleOp>(internalFixture, &context);
    auto owner = *candidate->getOps<CircuitOp>().begin();
    SmallVector<Attribute> input, expected;
    for (auto attr : internalAnnotations) {
      Annotation changed(attr); changed.setMember("class", b.getStringAttr(klass));
      input.push_back(changed.getAttr());
    }
    for (auto attr : ia) {
      Annotation changed(attr); changed.setMember("class", b.getStringAttr(klass));
      expected.push_back(changed.getAttr());
    }
    owner->setAttr("rawAnnotations", b.getArrayAttr(input));
    error.clear();
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*candidate, owner, error)) &&
            succeeded(verify(*candidate)) && owner->getAttr("rawAnnotations") == b.getArrayAttr(expected),
            "single-target internal fanout/identity/payload changed: " + error);
    InnerSymbolTable::walkSymbols(*owner.getOps<FModuleOp>().begin(), [&](StringAttr, InnerSymTarget) {
      require(false, "temporary host global identity leaked");
    });
    before = dump(*candidate);
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*candidate, owner, error)) &&
            dump(*candidate) == before, "host global lowering is not idempotent");
    for (StringRef bad : {"~Top|Top>absent", "~Top|Top>state.absent", "~Top|Top>agg.v[1]"}) {
      auto invalid = parseSourceString<ModuleOp>(internalFixture, &context);
      auto invalidOwner = *invalid->getOps<CircuitOp>().begin();
      invalidOwner->setAttr("rawAnnotations", b.getArrayAttr({input.front(), annotation(bad, "bad", klass)}));
      before = dump(*invalid);
      require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, invalidOwner, error)) &&
              dump(*invalid) == before, "invalid host global target mutated IR");
    }
  }
  for (auto bad : {annotation("~Top|Top>agg.v[1]", "bad"),
                   annotation("~Top|Top>state.absent", "bad"),
                   annotation("~Top|Top>alias.a", "bad-event",
                              goldengate::AnnotationClasses::AutoCounter)}) {
    auto invalid = parseSourceString<ModuleOp>(internalFixture, &context);
    auto circuit = *invalid->getOps<CircuitOp>().begin();
    circuit->setAttr("rawAnnotations", b.getArrayAttr({internalAnnotations.front(), bad}));
    before = dump(*invalid);
    require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, circuit, error)) &&
                dump(*invalid) == before, "invalid internal selector mutated IR");
  }
  // Clock/reset selectors must follow the same exact leaf identities as the
  // event. Collide aggregate leaves with scalar ports and internal declarations
  // so a simple underscore substitution would bind the wrong value.
  const char *counterFixture = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %io: !firrtl.bundle<event: uint<8>, domain: vector<bundle<clock: clock, reset: uint<1>>, 1>>,
                        in %io_domain_0_clock: !firrtl.clock,
                        in %io_domain_0_reset: !firrtl.uint<2>,
                        in %state_clock: !firrtl.clock,
                        in %state_reset: !firrtl.uint<3>) {
        %state = firrtl.node %io : !firrtl.bundle<event: uint<8>, domain: vector<bundle<clock: clock, reset: uint<1>>, 1>>
        %state_domain_0_clock = firrtl.wire : !firrtl.clock
        %state_domain_0_reset = firrtl.wire : !firrtl.uint<4>
      }
    }
  })mlir";
  auto counter = parseSourceString<ModuleOp>(counterFixture, &context);
  require(bool(counter), "AutoCounter selector fixture parse failed");
  auto ac = *counter->getOps<CircuitOp>().begin();
  auto counterAnnotation = [&](StringRef event, StringRef clock, StringRef reset,
                               StringRef klass = goldengate::AnnotationClasses::AutoCounter) {
    Annotation result(annotation(event, "counter", klass));
    result.setMember("clock", b.getStringAttr(clock));
    result.setMember("reset", b.getStringAttr(reset));
    result.setMember("label", b.getStringAttr("counter"));
    return result.getAttr();
  };
  SmallVector<Attribute> counters{
      counterAnnotation("~Top|Top>io.event", "~Top|Top>io.domain[0].clock",
                        "~Top|Top>io.domain.0.reset"),
      counterAnnotation("~Top|Top>state.event", "~Top|Top>state.domain.0.clock",
                        "~Top|Top>state.domain[0].reset"),
      counterAnnotation("~Top|Top>io.event", "~Top|Top>io_domain_0_clock",
                        "~Top|Top>io_domain_0_reset"),
      counterAnnotation("~Top|Top>state.event", "~Top|Top>state_domain_0_clock",
                        "~Top|Top>state_domain_0_reset"),
      counterAnnotation("~Top|Top>io.event", "~Top|Top>io.domain[0].clock",
                        "~Top|Top>io.domain[0].reset",
                        goldengate::AnnotationClasses::InternalAutoCounter)};
  auto nativeClock = goldengate::resolveAnnotationTarget(
      ac, "~Top|Top>io.domain[0].clock", error);
  auto nativeReset = goldengate::resolveInternalFieldTarget(
      ac, "~Top|Top>state.domain[0].reset", error);
  require(nativeClock && nativeClock->port && nativeReset,
          "AutoCounter native leaf selectors did not resolve");
  nativeClock->module.setPortSymbolsAttr(*nativeClock->port,
      InnerSymAttr::get(&context,
          {property("counter_clock", *nativeClock->fieldID, "public")}));
  cast<InnerSymbolOpInterface>(nativeReset->declaration).setInnerSymbolAttr(
      InnerSymAttr::get(&context,
          {property("counter_reset", nativeReset->fieldID, "private")}));
  auto counterRefs = b.getArrayAttr({
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("counter_clock")),
      InnerRefAttr::get(b.getStringAttr("Top"), b.getStringAttr("counter_reset"))});
  ac->setAttr("test.stable_refs", counterRefs);
  ac->setAttr("rawAnnotations", b.getArrayAttr(counters));
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*counter, ac, error)), error);
  require(succeeded(verify(*counter)), "AutoCounter lowered fixture verification failed");
  auto counterRaw = ac->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(counterRaw.size() == counters.size(), "AutoCounter exact rename fanned out");
  SmallVector<goldengate::AutoCounterEvent> resolved;
  require(succeeded(goldengate::analyzeAutoCounterEvents(ac, resolved, error)) &&
              resolved.size() == 5, error);
  const unsigned resetWidths[] = {1, 1, 2, 4, 1};
  for (auto [i, event] : llvm::enumerate(resolved)) {
    require(cast<UIntType>(event.event.getType()).getWidth() == 8 &&
                isa<ClockType>(event.clock.getType()) &&
                cast<UIntType>(event.reset.getType()).getWidth() == resetWidths[i],
            "AutoCounter operand selected the wrong scalar leaf");
    if (i == 0 || i == 2 || i == 4)
      require(isa<BlockArgument>(event.event) && isa<BlockArgument>(event.clock) &&
                  isa<BlockArgument>(event.reset), "AutoCounter port reference became internal");
    else
      require(isa<NodeOp>(event.event.getDefiningOp()) &&
                  (i == 1 ? isa<NodeOp>(event.clock.getDefiningOp()) &&
                            isa<NodeOp>(event.reset.getDefiningOp()) :
                            isa<WireOp>(event.clock.getDefiningOp()) &&
                            isa<WireOp>(event.reset.getDefiningOp())),
              "AutoCounter internal declaration identity changed");
    Annotation result(counterRaw[i]);
    require(result.getMember<ArrayAttr>("test.payload") ==
                Annotation(counters[i]).getMember<ArrayAttr>("test.payload") &&
                result.getMember<StringAttr>("label").getValue() == "counter",
            "AutoCounter payload changed");
  }
  require(resolved[0].clock != resolved[2].clock &&
              resolved[1].clock != resolved[3].clock &&
              resolved[0].reset != resolved[2].reset &&
              resolved[1].reset != resolved[3].reset,
          "AutoCounter clock/reset collisions alias different identities");
  Annotation internalCounter(counterRaw[4]);
  Annotation publicCounter(counterRaw[0]);
  for (StringRef member : {"target", "clock", "reset"})
    require(internalCounter.getMember<StringAttr>(member) ==
                publicCounter.getMember<StringAttr>(member),
            "internal/public AutoCounter exact renames disagree");
  auto counterModule = *ac.getOps<FModuleOp>().begin();
  symbolCount = 0;
  InnerSymbolTable::walkSymbols(counterModule,
      [&](StringAttr, const InnerSymTarget &) { ++symbolCount; });
  require(symbolCount == 2 && ac->getAttr("test.stable_refs") == counterRefs,
          "AutoCounter temporary leaf symbols leaked or native InnerRefs changed");
  InnerSymbolTable counterSymbols(counterModule);
  auto clockIdentity = counterSymbols.lookup("counter_clock");
  auto resetIdentity = counterSymbols.lookup("counter_reset");
  require(clockIdentity && clockIdentity.isPort() && clockIdentity.getField() == 0 &&
              counterModule.getBodyBlock()->getArgument(clockIdentity.getPort()) == resolved[0].clock &&
              resetIdentity && !resetIdentity.isPort() && resetIdentity.getField() == 0 &&
              resetIdentity.getOp()->getResult(0) == resolved[1].reset,
          "native clock/reset symbols no longer refer to AutoCounter operands");
  require(counterModule.getPortSymbolAttr(clockIdentity.getPort()).getProps().front()
                  .getSymVisibility().getValue() == "public" &&
              cast<InnerSymbolOpInterface>(resetIdentity.getOp()).getInnerSymAttr()
                  .getProps().front().getSymVisibility().getValue() == "private",
          "native clock/reset symbol visibility changed");
  before = dump(*counter);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*counter, ac, error)) &&
              dump(*counter) == before, "AutoCounter exact renames are not idempotent");
  for (auto bad : {
      counterAnnotation("~Top|Top>io.event", "~Top|Top>io.domain", "~Top|Top>io.domain[0].reset"),
      counterAnnotation("~Top|Top>io.event", "~Top|Top>io.domain[1].clock", "~Top|Top>io.domain[0].reset"),
      counterAnnotation("~Top|Top>state.event", "~Top|Top>state.domain[0].clock", "~Top|Top>state.domain"),
      counterAnnotation("~Top|Top>state.event", "~Top|Top>state.domain[0].clock", "~Top|Top>state.domain[0].missing"),
      counterAnnotation("~Top|Top>io.event", "~Top|Top>absent", "~Top|Top>io.domain[0].reset")}) {
    auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
    auto circuit = *invalid->getOps<CircuitOp>().begin();
    circuit->setAttr("rawAnnotations", b.getArrayAttr({counters.front(), bad}));
    before = dump(*invalid);
    require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, circuit, error)) &&
                dump(*invalid) == before, "invalid AutoCounter clock/reset mutated IR");
  }

  // Trigger annotations use exact renames in the same production LowerTypes
  // boundary. Check both classes, sinks without reset, collisions and payload.
  using A = goldengate::AnnotationClasses;
  for (StringRef klass : {A::TriggerSource, A::InternalTriggerSource,
                          A::TriggerSink, A::InternalTriggerSink}) {
    auto candidate = parseSourceString<ModuleOp>(counterFixture, &context);
    auto circuit = *candidate->getOps<CircuitOp>().begin();
    bool source = klass == A::TriggerSource || klass == A::InternalTriggerSource;
    SmallVector<Attribute> triggers;
    for (unsigned i : {0u, 1u}) {
      Annotation trigger(counters[i]);
      trigger.setMember("class", b.getStringAttr(klass));
      if (source) trigger.setMember("sourceType", b.getBoolAttr(i == 0));
      else {
        NamedAttrList fields(trigger.getDict()); fields.erase("reset");
        trigger = Annotation(fields.getDictionary(&context));
      }
      triggers.push_back(trigger.getAttr());
    }
    circuit->setAttr("rawAnnotations", b.getArrayAttr(triggers));
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*candidate, circuit, error)), error);
    require(succeeded(verify(*candidate)), "lowered trigger selectors invalid");
    auto rewritten = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(rewritten.size() == 2, "trigger exact rename changed annotation count");
    for (unsigned i : {0u, 1u}) {
      NamedAttrList expected(Annotation(triggers[i]).getDict());
      for (StringRef member : {"target", "clock", "reset"})
        if (source || member != "reset")
          expected.set(member, Annotation(counterRaw[i]).getMember<StringAttr>(member));
      require(rewritten[i] == expected.getDictionary(&context),
              "trigger leaf identity, class, order or payload changed");
    }
    auto lowered = *circuit.getOps<FModuleOp>().begin(); unsigned symbols = 0;
    InnerSymbolTable::walkSymbols(lowered, [&](StringAttr, InnerSymTarget) { ++symbols; });
    require(symbols == 0, "temporary trigger identities leaked");
    before = dump(*candidate);
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*candidate, circuit, error)) &&
            dump(*candidate) == before, "trigger exact renames are not idempotent");
    for (StringRef member : {"target", "clock", "reset"}) {
      if (!source && member == "reset") continue;
      Annotation bad(triggers.front()); bad.setMember(member, b.getStringAttr("~Top|Top>io.domain"));
      auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
      auto owner = *invalid->getOps<CircuitOp>().begin();
      owner->setAttr("rawAnnotations", b.getArrayAttr({triggers.back(), bad.getAttr()}));
      before = dump(*invalid);
      require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
              dump(*invalid) == before, "aggregate trigger selector did not reject atomically");
    }
  }
  // All channel connections use exact per-endpoint renames, including
  // repetitions, optional clock and both directions. Data selectors must
  // follow leaf identity even when their flattened names collide.
  auto clockInfo = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(A::TargetClockChannel)),
      b.getNamedAttr("clockInfo", b.getArrayAttr({b.getDictionaryAttr({
          b.getNamedAttr("name", b.getStringAttr("base")),
          b.getNamedAttr("multiplier", b.getI64IntegerAttr(1)),
          b.getNamedAttr("divisor", b.getI64IntegerAttr(1))})})),
      b.getNamedAttr("perClockMFMR", b.getArrayAttr({b.getI64IntegerAttr(3)}))});
  for (int latency : {-3, -2, -1, 0, 3}) {
    bool pipe = latency >= 0;
    bool reverse = latency == -2;
    bool forward = latency == -3;
    auto info = pipe ? b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(A::PipeChannel)),
        b.getNamedAttr("latency", b.getI32IntegerAttr(latency))}) : reverse ? b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(A::DecoupledReverseChannel))}) : forward ? b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(A::DecoupledForwardChannel)),
        b.getNamedAttr("readySink", Annotation(counters[0]).getMember<StringAttr>("reset")),
        b.getNamedAttr("validSource", Annotation(counters[1]).getMember<StringAttr>("reset")),
        b.getNamedAttr("readySource", Annotation(counters[1]).getMember<StringAttr>("reset")),
        b.getNamedAttr("validSink", Annotation(counters[0]).getMember<StringAttr>("reset"))}) : clockInfo;
    StringRef endpointMember = pipe || reverse || forward ? "target" : "clock";
    auto channel = b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(A::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr("clock")),
        b.getNamedAttr("channelInfo", info),
        b.getNamedAttr("clock", Annotation(counters[1]).getMember<StringAttr>("clock")),
        b.getNamedAttr("sources", b.getArrayAttr({
            Annotation(counters[0]).getMember<StringAttr>(endpointMember),
            Annotation(counters[1]).getMember<StringAttr>(endpointMember),
            Annotation(counters[0]).getMember<StringAttr>(endpointMember)})),
        b.getNamedAttr("sinks", b.getArrayAttr({
            Annotation(counters[1]).getMember<StringAttr>(endpointMember),
            Annotation(counters[0]).getMember<StringAttr>(endpointMember)}))});
    auto channelCandidate = parseSourceString<ModuleOp>(counterFixture, &context);
    auto channelCircuit = *channelCandidate->getOps<CircuitOp>().begin();
    auto selected = goldengate::resolveAnnotationTarget(channelCircuit, pipe || reverse || forward ? "~Top|Top>io.event" : "~Top|Top>io.domain[0].clock", error);
    selected->module.setPortSymbolsAttr(*selected->port,
        InnerSymAttr::get(&context, {property("channel_endpoint", *selected->fieldID, "public")}));
    NamedAttrList emptyChannel(channel); emptyChannel.erase("clock");
    emptyChannel.erase("sources"); emptyChannel.set("sinks", b.getArrayAttr({}));
    if (forward)
      emptyChannel.set("channelInfo", b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(A::DecoupledForwardChannel))}));
    channelCircuit->setAttr("rawAnnotations", b.getArrayAttr({channel, emptyChannel.getDictionary(&context)}));
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*channelCandidate, channelCircuit, error)), error);
    NamedAttrList expectedChannel(channel);
    auto portEndpoint = Annotation(counterRaw[0]).getMember<StringAttr>(endpointMember);
    auto internalEndpoint = Annotation(counterRaw[1]).getMember<StringAttr>(endpointMember);
    if (forward) {
      NamedAttrList expectedInfo(info);
      for (StringRef member : {"readySink", "validSource", "readySource", "validSink"})
        expectedInfo.set(member, Annotation(counterRaw[member == "readySink" || member == "validSink" ? 0 : 1])
                                    .getMember<StringAttr>("reset"));
      expectedChannel.set("channelInfo", expectedInfo.getDictionary(&context));
    }
    expectedChannel.set("clock", Annotation(counterRaw[1]).getMember<StringAttr>("clock"));
    expectedChannel.set("sources", b.getArrayAttr({portEndpoint, internalEndpoint, portEndpoint}));
    expectedChannel.set("sinks", b.getArrayAttr({internalEndpoint, portEndpoint}));
    require(channelCircuit->getAttr("rawAnnotations") == b.getArrayAttr({
        expectedChannel.getDictionary(&context), emptyChannel.getDictionary(&context)}) &&
        succeeded(verify(*channelCandidate)), "channel identity/order/payload/options changed: " + info.getAs<StringAttr>("class").getValue().str());
    auto channelModule = *channelCircuit.getOps<FModuleOp>().begin(); unsigned channelSymbols = 0;
    InnerSymbolTable::walkSymbols(channelModule, [&](StringAttr name, InnerSymTarget target) {
      require(name.getValue() == "channel_endpoint" && target.isPort() &&
              channelModule.getPortName(target.getPort()) == portEndpoint.getValue().split('>').second,
              "channel did not preserve native leaf symbol");
      ++channelSymbols;
    });
    require(channelSymbols == 1, "temporary channel identities leaked");
    before = dump(*channelCandidate);
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*channelCandidate, channelCircuit, error)) &&
            dump(*channelCandidate) == before, "channel normalization is not idempotent");
    for (StringRef member : {"clock", "sources", "sinks"})
      for (StringRef spelling : {"~Top|Top>io.domain", "~Top|Top>state.domain[0]", "~Top|Top>absent"}) {
        NamedAttrList bad(channel);
        bad.set(member, member == "clock" ? Attribute(b.getStringAttr(spelling)) :
                Attribute(b.getArrayAttr({Annotation(counters[0]).getMember<StringAttr>(endpointMember),
                                          b.getStringAttr(spelling)})));
        auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
        auto owner = *invalid->getOps<CircuitOp>().begin();
        owner->setAttr("rawAnnotations", b.getArrayAttr({channel, bad.getDictionary(&context)}));
        before = dump(*invalid);
        require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
                !error.empty() && dump(*invalid) == before,
                "late channel endpoint must reject before identity materialization");
      }
    for (StringRef member : {"sources", "sinks"}) {
      NamedAttrList bad(channel);
      bad.set(member, b.getArrayAttr({Annotation(counters[0]).getMember<StringAttr>(endpointMember),
                                      b.getI64IntegerAttr(0)}));
      auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
      auto owner = *invalid->getOps<CircuitOp>().begin();
      owner->setAttr("rawAnnotations", b.getArrayAttr({bad.getDictionary(&context)}));
      before = dump(*invalid);
      require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
              error.find("endpoint is not a reference target") != std::string::npos &&
              dump(*invalid) == before, "malformed channel endpoint did not reject atomically");
    }
    if (forward)
      for (StringRef member : {"readySink", "validSource", "readySource", "validSink"})
        for (Attribute spelling : {Attribute(b.getStringAttr("~Top|Top>io.domain")),
                                   Attribute(b.getStringAttr("~Top|Top>state.domain[0]")),
                                   Attribute(b.getStringAttr("~Top|Top>absent")),
                                   Attribute(b.getI64IntegerAttr(0))}) {
          NamedAttrList badInfo(info); badInfo.set(member, spelling);
          NamedAttrList bad(channel); bad.set("channelInfo", badInfo.getDictionary(&context));
          auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
          auto owner = *invalid->getOps<CircuitOp>().begin();
          owner->setAttr("rawAnnotations", b.getArrayAttr({channel, bad.getDictionary(&context)}));
          before = dump(*invalid);
          require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
                  error.find(member.str()) != std::string::npos && dump(*invalid) == before,
                  "nested ready/valid endpoint must reject before identity materialization");
        }
  }
  // Local model channel groups use the same exact identity rule as connections.
  // Ordered repeated ports, absent clocks and empty groups survive normalization.
  auto modelPorts = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(A::ChannelPorts)),
      b.getNamedAttr("localName", b.getStringAttr("model_data")),
      b.getNamedAttr("test.payload", b.getI32IntegerAttr(7)),
      b.getNamedAttr("clockPort", Annotation(counters[1]).getMember<StringAttr>("clock")),
      b.getNamedAttr("ports", b.getArrayAttr({
          Annotation(counters[0]).getMember<StringAttr>("target"),
          Annotation(counters[1]).getMember<StringAttr>("target"),
          Annotation(counters[0]).getMember<StringAttr>("target")}))});
  auto modelCandidate = parseSourceString<ModuleOp>(counterFixture, &context);
  auto modelCircuit = *modelCandidate->getOps<CircuitOp>().begin();
  NamedAttrList emptyModel(modelPorts); emptyModel.erase("clockPort");
  emptyModel.set("ports", b.getArrayAttr({}));
  modelCircuit->setAttr("rawAnnotations", b.getArrayAttr({modelPorts, emptyModel.getDictionary(&context)}));
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*modelCandidate, modelCircuit, error)), error);
  NamedAttrList expectedModel(modelPorts);
  expectedModel.set("clockPort", Annotation(counterRaw[1]).getMember<StringAttr>("clock"));
  expectedModel.set("ports", b.getArrayAttr({
      Annotation(counterRaw[0]).getMember<StringAttr>("target"),
      Annotation(counterRaw[1]).getMember<StringAttr>("target"),
      Annotation(counterRaw[0]).getMember<StringAttr>("target")}));
  require(modelCircuit->getAttr("rawAnnotations") == b.getArrayAttr({
      expectedModel.getDictionary(&context), emptyModel.getDictionary(&context)}) &&
      succeeded(verify(*modelCandidate)), "local model channel target identity/order/payload/options changed");
  auto loweredModel = *modelCircuit.getOps<FModuleOp>().begin();
  InnerSymbolTable::walkSymbols(loweredModel, [&](StringAttr, InnerSymTarget) {
    require(false, "temporary model channel identity leaked");
  });
  before = dump(*modelCandidate);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*modelCandidate, modelCircuit, error)) &&
          dump(*modelCandidate) == before, "local model channel normalization is not idempotent");
  for (StringRef member : {"clockPort", "ports"})
    for (Attribute spelling : {Attribute(b.getStringAttr("~Top|Top>io.domain")),
                               Attribute(b.getStringAttr("~Top|Top>state.domain[0]")),
                               Attribute(b.getStringAttr("~Top|Top>absent")),
                               Attribute(b.getI64IntegerAttr(0))}) {
      NamedAttrList bad(modelPorts);
      bad.set(member, member == "clockPort" ? spelling : Attribute(b.getArrayAttr({
          Annotation(counters[0]).getMember<StringAttr>("target"), spelling})));
      auto invalid = parseSourceString<ModuleOp>(counterFixture, &context);
      auto owner = *invalid->getOps<CircuitOp>().begin();
      owner->setAttr("rawAnnotations", b.getArrayAttr({modelPorts, bad.getDictionary(&context)}));
      before = dump(*invalid);
      require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
              error.find(member.str()) != std::string::npos && dump(*invalid) == before,
              "local model channel must reject before identity materialization");
    }
  // Unused sources/sinks are consumed without resolving their ground metadata.
  auto unused = parseSourceString<ModuleOp>(counterFixture, &context);
  auto unusedCircuit = *unused->getOps<CircuitOp>().begin();
  auto unresolved = counterAnnotation("~Top|Top>absent", "~Top|Top>absentClock",
                                      "~Top|Top>absentReset", A::InternalTriggerSource);
  unusedCircuit->setAttr("rawAnnotations", b.getArrayAttr({unresolved}));
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*unused, unusedCircuit, error)), error);
  require(unusedCircuit->getAttr("rawAnnotations") == b.getArrayAttr({unresolved}),
          "unresolved ground trigger metadata changed during normalization");
  llvm::outs() << "DontTouch aggregates expand in declaration order; fields, flips and unrelated targets preserved; three invalid selectors reject atomically; namespace collisions follow leaf identities and preserve native InnerRefs without temporary symbol leakage; internal wire/node/register targets expand and follow declaration namespace renames; AutoCounter event/clock/reset references preserve port and internal leaf identities\n";

}
// Compare the native normalization boundary to Scala LowForm using the same
// data selectors on aggregate ports, nodes and wires. Emit only when requested.
void channelTargets(MLIRContext &context, unsigned mode, unsigned kind, StringRef output) {
  bool reverse = kind == 1;
  bool forward = kind == 2;
  bool modelPorts = kind == 3;
  std::string text = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %io: !firrtl.bundle<clock: clock, rows: vector<bundle<data: uint<8>, flag: uint<1>>, 2>>,
                        in %scalar: !firrtl.uint<8>) {
  )mlir";
  text += mode == 2 ? R"mlir(
        %base = firrtl.wire : !firrtl.bundle<clock: clock, rows: vector<bundle<data: uint<8>, flag: uint<1>>, 2>>
        firrtl.strictconnect %base, %io : !firrtl.bundle<clock: clock, rows: vector<bundle<data: uint<8>, flag: uint<1>>, 2>>
  )mlir" : R"mlir(
        %base = firrtl.node %io : !firrtl.bundle<clock: clock, rows: vector<bundle<data: uint<8>, flag: uint<1>>, 2>>
  )mlir";
  text += "} } }";
  if (reverse) {
    auto scalar = text.find("in %scalar: !firrtl.uint<8>");
    text.replace(scalar, std::string("in %scalar: !firrtl.uint<8>").size(),
                 "in %scalar: !firrtl.uint<1>");
  }
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "channel normalization oracle fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  std::string name = mode == 0 ? "io" : "base";
  auto target = [&](StringRef field) { return b.getStringAttr("~Top|Top>" + name + field.str()); };
  auto info = reverse ? b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledReverseChannel))}) : forward ?
      b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledForwardChannel)),
          b.getNamedAttr("readySink", target(".rows[0].flag")),
          b.getNamedAttr("validSource", target(".rows[1].flag")),
          b.getNamedAttr("readySource", target(".rows[1].flag")),
          b.getNamedAttr("validSink", target(".rows[0].flag"))}) : b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::PipeChannel)),
          b.getNamedAttr("latency", b.getI32IntegerAttr(mode))});
  std::string field = reverse ? "flag" : "data";
  auto channel = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelConnection)),
      b.getNamedAttr("globalName", b.getStringAttr("data")),
      b.getNamedAttr("channelInfo", info),
      b.getNamedAttr("clock", target(".clock")),
      b.getNamedAttr("sources", b.getArrayAttr({target(".rows[1]." + field),
          b.getStringAttr("~Top|Top>scalar"), target(".rows[0]." + field), target(".rows[1]." + field)})),
      b.getNamedAttr("sinks", b.getArrayAttr({target(".rows[0].flag"), target(".rows[1]." + field)}))});
  NamedAttrList empty(channel); empty.erase("clock"); empty.erase("sources");
  empty.set("globalName", b.getStringAttr("empty")); empty.set("sinks", b.getArrayAttr({}));
  if (forward)
    empty.set("channelInfo", b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::DecoupledForwardChannel))}));
  if (modelPorts) {
    channel = b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::ChannelPorts)),
        b.getNamedAttr("localName", b.getStringAttr("data")),
        b.getNamedAttr("clockPort", target(".clock")),
        b.getNamedAttr("ports", Annotation(channel).getMember<ArrayAttr>("sources"))});
    empty = NamedAttrList(channel); empty.erase("clockPort");
    empty.set("localName", b.getStringAttr("empty")); empty.set("ports", b.getArrayAttr({}));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr({channel, empty.getDictionary(&context)}));
  std::string error;
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
  require(succeeded(verify(*root)), "channel normalization oracle produced invalid IR");
  NamedAttrList expected(channel);
  if (forward) {
    NamedAttrList expectedInfo(info);
    expectedInfo.set("readySink", target("_rows_0_flag"));
    expectedInfo.set("validSource", target("_rows_1_flag"));
    expectedInfo.set("readySource", target("_rows_1_flag"));
    expectedInfo.set("validSink", target("_rows_0_flag"));
    expected.set("channelInfo", expectedInfo.getDictionary(&context));
  }
  expected.set("clock", target("_clock"));
  expected.set("sources", b.getArrayAttr({target("_rows_1_" + field), b.getStringAttr("~Top|Top>scalar"),
                                        target("_rows_0_" + field), target("_rows_1_" + field)}));
  expected.set("sinks", b.getArrayAttr({target("_rows_0_flag"), target("_rows_1_" + field)}));
  if (modelPorts) {
    expected.erase("clock"); expected.erase("sources"); expected.erase("sinks");
    expected.set("clockPort", target("_clock"));
    expected.set("ports", b.getArrayAttr({target("_rows_1_data"), b.getStringAttr("~Top|Top>scalar"),
                                        target("_rows_0_data"), target("_rows_1_data")}));
  }
  require(circuit->getAttr("rawAnnotations") == b.getArrayAttr({
      expected.getDictionary(&context), empty.getDictionary(&context)}),
      "channel oracle endpoints/payload/options changed");
  if (!output.empty()) {
    std::error_code ec;
    llvm::raw_fd_ostream file(output, ec);
    require(!ec, "cannot write channel normalization fixture: " + ec.message());
    root->print(file);
  }
}
void hostTargets(MLIRContext &context, unsigned mode, bool wiring, StringRef output) {
  std::string text = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %io: !firrtl.bundle<clocks: vector<clock, 2>, resets: vector<uint<1>, 2>>,
                        in %empty: !firrtl.bundle<>) {
  )mlir";
  text += mode == 2 ? R"mlir(
        %base = firrtl.wire : !firrtl.bundle<clocks: vector<clock, 2>, resets: vector<uint<1>, 2>>
        firrtl.strictconnect %base, %io : !firrtl.bundle<clocks: vector<clock, 2>, resets: vector<uint<1>, 2>>
  )mlir" : R"mlir(
        %base = firrtl.node %io : !firrtl.bundle<clocks: vector<clock, 2>, resets: vector<uint<1>, 2>>
  )mlir";
  text += "} } }";
  auto root = parseSourceString<ModuleOp>(text, &context);
  require(bool(root), "host global oracle fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  std::string name = mode == 0 ? "io" : "base";
  auto annotation = [&](StringRef klass, StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass)),
        b.getNamedAttr("target", b.getStringAttr("~Top|Top>" + target.str()))});
  };
  using A = goldengate::AnnotationClasses;
  StringRef source = wiring ? A::HostClockSource : A::HostClock;
  StringRef sink = wiring ? A::HostClockSink : A::HostReset;
  std::string sinkField = wiring ? "clocks" : "resets";
  circuit->setAttr("rawAnnotations", b.getArrayAttr({
      annotation(source, name + ".clocks"), annotation(sink, name + "." + sinkField),
      annotation(source, name + ".clocks[1]"), annotation(sink, "empty")}));
  std::string error;
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
  require(succeeded(verify(*root)) && circuit->getAttr("rawAnnotations") == b.getArrayAttr({
      annotation(source, name + "_clocks_0"), annotation(source, name + "_clocks_1"),
      annotation(sink, name + "_" + sinkField + "_0"), annotation(sink, name + "_" + sinkField + "_1")}),
      "host global fanout/empty target/duplicate coalescing changed");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file(output, ec);
    require(!ec, "cannot write host global oracle: " + ec.message()); root->print(file);
  }
}
void debugTargets(MLIRContext &context, bool internal, bool legacy, StringRef output) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %clock: !firrtl.clock, in %reset: !firrtl.uint<1>,
          in %io: !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>,
          in %empty: !firrtl.bundle<>) {
        %alias = firrtl.node %io : !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>
        %wire = firrtl.wire : !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>
        firrtl.strictconnect %wire, %io : !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>
        %state = firrtl.reg %clock : !firrtl.clock, !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>
        firrtl.strictconnect %state, %io : !firrtl.bundle<a: uint<8>, nested: bundle<ready: uint<1>, data: vector<uint<4>, 2>>>
      }
    }
  })mlir", &context);
  require(bool(root), "FPGA debug fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto annotation = [&](StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(internal ?
        goldengate::AnnotationClasses::InternalFpgaDebug : goldengate::AnnotationClasses::FpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr((legacy ? "Top.Top." : "~Top|Top>") + target.str()))});
  };
  SmallVector<Attribute> input, expected;
  for (StringRef name : {"io", "alias", "wire", "state"}) {
    input.push_back(annotation(name));
    input.push_back(annotation(name.str() + ".nested.data[1]"));
    for (StringRef suffix : {"_a", "_nested_ready", "_nested_data_0", "_nested_data_1"})
      expected.push_back(annotation(name.str() + suffix.str()));
  }
  input.push_back(annotation("empty"));
  // Legacy ComponentName needs complete circuit/module/reference and selectors
  // must resolve before any previously valid annotation is expanded.
  for (StringRef bad : {"Other.Top.io", "Top..io", "Top.Top.",
                        "Top.Top.io.nested.data[2]", "Top.Missing.io"}) {
    auto invalid = cast<ModuleOp>(root->clone());
    auto owner = *invalid.getOps<CircuitOp>().begin();
    Annotation broken(input.front()); broken.setMember("target", b.getStringAttr(bad));
    owner->setAttr("rawAnnotations", b.getArrayAttr({input.front(), broken.getAttr()}));
    auto before = dump(invalid); std::string error;
    require(failed(goldengate::lowerTypesWithRetainedTargets(invalid, owner, error)) &&
            !error.empty() && dump(invalid) == before, "invalid debug ComponentName mutated IR");
    invalid.erase();
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(input));
  std::string error;
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
  require(succeeded(verify(*root)) && circuit->getAttr("rawAnnotations") == b.getArrayAttr(expected),
          "FPGA debug fanout, duplicate coalescing or ComponentName representation changed");
  auto before = dump(*root);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)) &&
          dump(*root) == before, "debug target lowering is not idempotent");
  if (!output.empty()) {
    std::error_code ec; llvm::raw_fd_ostream file(output, ec);
    require(!ec, "cannot write FPGA debug fixture: " + ec.message()); root->print(file);
  }
}
// SFC RemoveZeroWidth drops fanout leaves and makes exact renames fail.
void memoryPortTargets(MLIRContext &context) {
  const char *fixture = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %io: !firrtl.bundle<data: uint<8>, mask: uint<1>, addr: uint<2>, en: uint<1>, wmode: uint<1>, rdata: uint<8>, wdata: vector<uint<8>, 2>, wmask: uint<1>, zero: uint<0>>,
                        in %io_data: !firrtl.uint<19>) {
        %alias = firrtl.node %io : !firrtl.bundle<data: uint<8>, mask: uint<1>, addr: uint<2>, en: uint<1>, wmode: uint<1>, rdata: uint<8>, wdata: vector<uint<8>, 2>, wmask: uint<1>, zero: uint<0>>
        %wire = firrtl.wire : !firrtl.bundle<data: uint<8>, mask: uint<1>, addr: uint<2>, en: uint<1>, wmode: uint<1>, rdata: uint<8>, wdata: vector<uint<8>, 2>, wmask: uint<1>, zero: uint<0>>
        firrtl.strictconnect %wire, %io : !firrtl.bundle<data: uint<8>, mask: uint<1>, addr: uint<2>, en: uint<1>, wmode: uint<1>, rdata: uint<8>, wdata: vector<uint<8>, 2>, wmask: uint<1>, zero: uint<0>>
      }
    }
  })mlir";
  OpBuilder b(&context);
  const StringRef classes[]{goldengate::AnnotationClasses::ModelReadPort,
      goldengate::AnnotationClasses::ModelWritePort,
      goldengate::AnnotationClasses::ModelReadWritePort};
  const SmallVector<StringRef> members[]{
      {"data", "addr", "en"}, {"data", "mask", "addr", "en"},
      {"wmode", "rdata", "wdata", "wmask", "addr", "en"}};
  auto annotations = [&](StringRef declaration) {
    SmallVector<DictionaryAttr> result;
    for (unsigned kind = 0; kind < 3; ++kind) {
      NamedAttrList attrs;
      attrs.set("class", b.getStringAttr(classes[kind]));
      attrs.set("test.payload", b.getArrayAttr({b.getI32IntegerAttr(42)}));
      for (auto member : members[kind])
        attrs.set(member, b.getStringAttr("~Top|Top>" + declaration.str() +
            "." + member.str() + (member == "wdata" ? "[1]" : "")));
      result.push_back(attrs.getDictionary(&context));
    }
    return result;
  };
  std::string error;
  for (StringRef declaration : {"io", "alias", "wire"}) {
    auto root = parseSourceString<ModuleOp>(fixture, &context);
    require(bool(root), "memory model selector fixture parse failed");
    auto circuit = *root->getOps<CircuitOp>().begin();
    auto original = annotations(declaration);
    circuit->setAttr("rawAnnotations", b.getArrayAttr(SmallVector<Attribute>(original.begin(), original.end())));
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
    require(succeeded(verify(*root)), "memory model selector lowering produced invalid IR");
    auto lowered = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    require(lowered.size() == 3, "memory port annotations fanned out or disappeared");
    for (unsigned kind = 0; kind < 3; ++kind) {
      Annotation result(lowered[kind]);
      require(result.isClass(classes[kind]) &&
              result.getMember<ArrayAttr>("test.payload") == Annotation(original[kind]).getMember<ArrayAttr>("test.payload"),
              "memory annotation class/order/payload changed");
      for (auto member : members[kind]) {
        auto expected = "~Top|Top>" + declaration.str() + "_" + member.str() +
            (member == "wdata" ? "_1" : "");
        auto spelling = result.getMember<StringAttr>(member);
        require(spelling && spelling.getValue() == expected,
                "memory member identity: expected " + expected + ", got " +
                (spelling ? spelling.getValue().str() : "<absent>"));
        if (declaration == "io") {
          auto target = goldengate::resolveAnnotationTarget(circuit, spelling.getValue(), error);
          require(target && target->port && *target->fieldID == 0, "memory port member does not resolve to a ground port");
          if (member == "data")
            require(cast<UIntType>(target->module.getPortType(*target->port)).getWidth() == 8,
                    "memory data target selected the colliding 19-bit port");
        } else {
          auto target = goldengate::resolveInternalFieldTarget(circuit, spelling.getValue(), error);
          require(target && target->fieldID == 0 && target->type.isGround(),
                  "internal memory member does not resolve to a ground declaration");
        }
      }
    }
    for (auto owner : circuit.getOps<FModuleLike>())
      circt::hw::InnerSymbolTable::walkSymbols(owner, [&](StringAttr, circt::hw::InnerSymTarget) {
        require(false, "memory member lowering leaked temporary symbols");
      });
    auto before = dump(*root);
    require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)) && dump(*root) == before,
            "memory annotation target transfer is not idempotent");
  }
  auto original = annotations("io");
  // SFC exact renaming accepts an aggregate with one surviving leaf. The
  // memory port classes describe payloads, so single-element vector selections
  // must resolve to that leaf rather than rejecting their input syntax.
  auto singleton = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(in %data: !firrtl.bundle<only: vector<uint<8>, 1>>,
                        in %mask: !firrtl.bundle<only: uint<1>>,
                        in %addr: !firrtl.uint<2>, in %en: !firrtl.uint<1>) {}
    }
  })mlir", &context);
  auto sc = *singleton->getOps<CircuitOp>().begin();
  NamedAttrList singletonAttrs;
  singletonAttrs.set("class", b.getStringAttr(classes[1]));
  for (StringRef member : {"data", "mask", "addr", "en"})
    singletonAttrs.set(member, b.getStringAttr("~Top|Top>" + member.str()));
  sc->setAttr("rawAnnotations", b.getArrayAttr({singletonAttrs.getDictionary(&context)}));
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*singleton, sc, error)) && succeeded(verify(*singleton)), error);
  Annotation single(sc->getAttrOfType<ArrayAttr>("rawAnnotations")[0]);
  require(single.getMember<StringAttr>("data").getValue() == "~Top|Top>data_only_0" &&
          single.getMember<StringAttr>("mask").getValue() == "~Top|Top>mask_only",
          "singleton memory data/mask aggregates did not follow their sole leaf");
  for (unsigned kind = 0; kind < 3; ++kind)
    for (auto member : members[kind])
      for (unsigned bad = 0; bad < 5; ++bad) {
        auto root = parseSourceString<ModuleOp>(fixture, &context);
        auto circuit = *root->getOps<CircuitOp>().begin();
        NamedAttrList invalid(original[kind]);
        if (bad == 0) invalid.set(member, b.getStringAttr("~Top|Top>io"));
        if (bad == 1) invalid.set(member, b.getStringAttr("~Top|Top>io.zero"));
        if (bad == 2) invalid.set(member, b.getStringAttr("~Top|Top>io.absent"));
        if (bad == 3) invalid.erase(member);
        if (bad == 4) invalid.set(member, b.getI32IntegerAttr(3));
        circuit->setAttr("rawAnnotations", b.getArrayAttr({original.front(), invalid.getDictionary(&context)}));
        auto before = dump(*root); error.clear();
        require(failed(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)) &&
                !error.empty() && dump(*root) == before,
                "invalid memory member accepted or mutated IR before rejection");
      }
  // Late debug-only lowering must preserve historical SRAM metadata without
  // resolving ports that its earlier consumer may already have removed.
  auto late = parseSourceString<ModuleOp>(fixture, &context);
  auto lateCircuit = *late->getOps<CircuitOp>().begin();
  SmallVector<Attribute> historical;
  for (auto klass : classes)
    historical.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass))}));
  auto raw = b.getArrayAttr(historical);
  lateCircuit->setAttr("rawAnnotations", raw);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*late, lateCircuit, error,
              goldengate::RetainedTargetScope::FpgaDebugOnly)) &&
          lateCircuit->getAttr("rawAnnotations") == raw && succeeded(verify(*late)),
          "debug-only normalization validated or rewrote historical SRAM metadata");
  llvm::outs() << "SRAM port annotations: 39 port/node/wire member identities, colliding names, vector elements, singleton aggregates, idempotence, symbol cleanup, 65 atomic rejections and late debug-only preservation passed\n";
}
void zeroWidthTargets(MLIRContext &context) {
  const char *fixture = R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock,
                        in %io: !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>,
                        in %empty: !firrtl.bundle<pad: uint<0>, signed: sint<0>>) {
        %alias = firrtl.node %io : !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        %wire = firrtl.wire : !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        firrtl.strictconnect %wire, %io : !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        %state = firrtl.reg %clock : !firrtl.clock, !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        firrtl.strictconnect %state, %io : !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        %pad = firrtl.subfield %io[pad] : !firrtl.bundle<pad: uint<0>, signed: sint<0>, valid: uint<1>>
        %inferred = firrtl.wire : !firrtl.uint
        firrtl.connect %inferred, %pad : !firrtl.uint, !firrtl.uint<0>
      }
    }
  })mlir";
  using A = goldengate::AnnotationClasses;
  OpBuilder b(&context);
  auto single = [&](StringRef klass, StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass)),
        b.getNamedAttr("target", b.getStringAttr("~Top|Top>" + target.str()))});
  };
  SmallVector<Attribute> fanout, expected;
  for (StringRef klass : {A::DontTouch, A::HostReset, A::HostClockSource,
                         A::GlobalResetSink, A::FpgaDebug}) {
    for (StringRef name : {"io", "alias", "wire", "state"}) {
      fanout.push_back(single(klass, name));
      fanout.push_back(single(klass, name.str() + ".pad"));
      fanout.push_back(single(klass, name.str() + ".signed"));
      expected.push_back(single(klass, name.str() + "_valid"));
    }
    fanout.push_back(single(klass, "empty"));
    fanout.push_back(single(klass, "inferred"));
  }
  auto root = parseSourceString<ModuleOp>(fixture, &context);
  require(bool(root), "zero-width fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  circuit->setAttr("rawAnnotations", b.getArrayAttr(fanout));
  std::string error;
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)), error);
  require(succeeded(verify(*root)) && circuit->getAttr("rawAnnotations") == b.getArrayAttr(expected),
          "zero-width fanout annotations differ from SFC deletion semantics");
  auto before = dump(*root);
  require(succeeded(goldengate::lowerTypesWithRetainedTargets(*root, circuit, error)) &&
          dump(*root) == before, "zero-width fanout is not idempotent");
  SmallVector<DictionaryAttr> exact;
  auto ref = b.getStringAttr("~Top|Top>io.pad");
  auto valid = b.getStringAttr("~Top|Top>io.valid");
  for (StringRef member : {"ports", "clockPort"}) {
    NamedAttrList attrs;
    attrs.set("class", b.getStringAttr(A::ChannelPorts));
    attrs.set("localName", b.getStringAttr("payload"));
    attrs.set("ports", b.getArrayAttr({valid}));
    attrs.set(member, member == "ports" ? Attribute(b.getArrayAttr({valid, ref})) : Attribute(ref));
    exact.push_back(attrs.getDictionary(&context));
  }
  for (StringRef member : {"clock", "sources", "sinks", "readySink", "validSource", "readySource", "validSink"}) {
    NamedAttrList info, attrs;
    info.set("class", b.getStringAttr(A::DecoupledForwardChannel));
    attrs.set("class", b.getStringAttr(A::ChannelConnection));
    attrs.set("globalName", b.getStringAttr("payload"));
    if (member == "clock") attrs.set(member, ref);
    else if (member == "sources" || member == "sinks") attrs.set(member, b.getArrayAttr({valid, ref}));
    else info.set(member, ref);
    attrs.set("channelInfo", info.getDictionary(&context));
    exact.push_back(attrs.getDictionary(&context));
  }
  for (StringRef klass : {A::AutoCounter, A::TriggerSource, A::TriggerSink})
    for (StringRef member : {"target", "clock", "reset"}) {
      if (klass == A::TriggerSink && member == "reset") continue;
      NamedAttrList attrs(single(klass, "io.valid")); attrs.set(member, ref);
      exact.push_back(attrs.getDictionary(&context));
    }
  for (auto bad : exact) {
    for (StringRef target : {"io.pad", "alias.signed", "wire.pad", "state.signed"}) {
      // Replace every occurrence of the invalid reference, including nested info.
      NamedAttrList attrs(bad);
      for (auto attr : bad) {
        auto replacement = b.getStringAttr("~Top|Top>" + target.str());
        if (attr.getValue() == ref) attrs.set(attr.getName(), replacement);
        if (auto array = dyn_cast<ArrayAttr>(attr.getValue())) {
          SmallVector<Attribute> values(array.begin(), array.end());
          for (auto &value : values) if (value == ref) value = replacement;
          attrs.set(attr.getName(), b.getArrayAttr(values));
        }
        if (auto dict = dyn_cast<DictionaryAttr>(attr.getValue())) {
          NamedAttrList nested(dict);
          for (auto field : dict) if (field.getValue() == ref) nested.set(field.getName(), replacement);
          attrs.set(attr.getName(), nested.getDictionary(&context));
        }
      }
      auto invalid = parseSourceString<ModuleOp>(fixture, &context);
      auto owner = *invalid->getOps<CircuitOp>().begin();
      owner->setAttr("rawAnnotations", b.getArrayAttr({fanout.front(), attrs.getDictionary(&context)}));
      before = dump(*invalid); error.clear();
      require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
              error.find("zero-width") != std::string::npos && dump(*invalid) == before,
              "explicit zero-width exact target did not reject atomically");
    }
  }
  // An unknown input width must be checked after InferWidths, with raw
  // annotations unpublished and every temporary identity cleaned on failure.
  auto invalid = parseSourceString<ModuleOp>(fixture, &context);
  auto owner = *invalid->getOps<CircuitOp>().begin();
  NamedAttrList inferred(exact.front());
  inferred.set("ports", b.getArrayAttr({valid, b.getStringAttr("~Top|Top>inferred")}));
  auto raw = b.getArrayAttr({fanout.front(), inferred.getDictionary(&context)});
  owner->setAttr("rawAnnotations", raw); error.clear();
  require(failed(goldengate::lowerTypesWithRetainedTargets(*invalid, owner, error)) &&
          error.find("inferred zero-width") != std::string::npos &&
          owner->getAttr("rawAnnotations") == raw && succeeded(verify(*invalid)),
          "inferred zero-width exact target accepted or published annotations");
  auto noTemporarySymbols = [](CircuitOp c) {
    for (auto module : c.getOps<FModuleLike>())
      circt::hw::InnerSymbolTable::walkSymbols(module, [&](StringAttr, circt::hw::InnerSymTarget) {
        require(false, "zero-width lowering leaked temporary inner symbols");
      });
  };
  noTemporarySymbols(circuit); noTemporarySymbols(owner);
  llvm::outs() << "Zero-width fanout leaves deleted for five annotation classes; inferred zeros deleted; 68 exact endpoint cases reject atomically; inferred exact endpoint rejects without publishing annotations or leaking symbols\n";
}
} // namespace
int main(int argc, char **argv) {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    run(context);
    memoryPortTargets(context);
    zeroWidthTargets(context);
    for (bool internal : {false, true})
      for (bool legacy : {false, true}) {
        std::string name = internal ? "internal" : "public";
        if (!legacy) name += "-modern";
        debugTargets(context, internal, legacy, argc > 1 ? std::string(argv[1]) + "/" + name + "-candidate.mlir" : "");
      }
    for (unsigned mode = 0; mode < 3; ++mode) {
      StringRef name = mode == 0 ? "port-fields" : mode == 1 ? "node-fields" : "wire-fields";
      hostTargets(context, mode, false, argc > 1 ? std::string(argv[1]) + "/host-" + name.str() + "-candidate.mlir" : "");
      hostTargets(context, mode, true, argc > 1 ? std::string(argv[1]) + "/host-wiring-" + name.str() + "-candidate.mlir" : "");
      for (unsigned kind = 0; kind < 4; ++kind) {
        std::string prefix = kind == 1 ? "reverse-" : kind == 2 ? "forward-" : kind == 3 ? "model-" : "";
        channelTargets(context, mode, kind, argc > 1 ? std::string(argv[1]) + "/" + prefix + name.str() + "-candidate.mlir" : "");
      }
    }
  }
  catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
  return 0;
}
