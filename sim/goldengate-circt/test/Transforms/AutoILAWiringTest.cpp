// See LICENSE for license details.
#include "goldengate/AutoILAWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/HostClockWiring.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  throw std::runtime_error("missing module " + name.str());
}
Value driver(FModuleOp module, Value dest) {
  Value result;
  for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>())
    if (connect.getDest() == dest) { require(!result, "multiple probe drivers"); result = connect.getSrc(); }
  require(bool(result), "missing probe driver"); return result;
}
OwningOpRef<ModuleOp> fixture(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %a: !firrtl.uint<1>, in %ila_right_child_signed: !firrtl.uint<1>) {
        %b = firrtl.wire : !firrtl.uint<8>
        %right_a = firrtl.instance right @Mid(out a: !firrtl.uint<1>)
        %left_a = firrtl.instance left @Mid(out a: !firrtl.uint<1>)
        %oldUse = firrtl.node %right_a : !firrtl.uint<1>
      }
      firrtl.module private @Mid(out %a: !firrtl.uint<1>) {
        %child_a = firrtl.instance child @Leaf(out a: !firrtl.uint<1>)
      }
      firrtl.module private @Leaf(out %a: !firrtl.uint<1>) {
        %signed = firrtl.wire : !firrtl.sint<7>
        %node = firrtl.node %a : !firrtl.uint<1>
        %ila_signed = firrtl.wire : !firrtl.uint<1>
      }
      firrtl.module private @Unused(in %u: !firrtl.uint<3>) {
        %unused_a = firrtl.instance unused @Mid(out a: !firrtl.uint<1>)
      }
    }
  })mlir", &context);
  require(bool(root), "fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto target : {"Top.Leaf.a", "Top.Top.a", "Top.Leaf.node", "Top.Top.b",
                      "Top.Leaf.signed", "~Top|Leaf>signed", "Top.Mid.a"})
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr(target))}));
  annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr("example.Keep"))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  for (auto module : circuit.getOps<FModuleOp>())
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>())
      instance->setAttr("example.metadata", b.getStringAttr("keep"));
  return root;
}
void routing(MLIRContext &context) {
  auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top"); auto raw = circuit->getAttr("rawAnnotations");
  auto oldUse = *top.getBodyBlock()->getOps<NodeOp>().begin();
  SmallVector<goldengate::WiredILAProbe> outputs; std::string error;
  require(succeeded(goldengate::wireAutoILAProbesToTop(circuit, outputs, error)), error);
  require(outputs.size() == 10 && top.getNumPorts() == 12,
          "wrong probe fanout or original top ports lost");
  require(circuit->getAttr("rawAnnotations") == raw, "pending annotations changed");
  require(named(circuit, "Leaf").getNumPorts() == 4 && named(circuit, "Mid").getNumPorts() == 5 &&
          named(circuit, "Unused").getNumPorts() == 5, "shared/unused parent signatures changed incorrectly");
  std::set<std::string> names;
  for (auto &output : outputs) {
    auto &probe = output.source;
    require(probe.index == names.size() && output.topPort == top.getBodyBlock()->getArgument(2 + probe.index),
            "probe/port index changed");
    auto name = top.getPortName(2 + probe.index);
    require(names.insert(name.str()).second && output.topTarget == "~Top|Top>" + name.str(),
            "top name collision or target mismatch");
    require(output.topPort.getType() == probe.value.getType(), "probe type changed");
    Value value = output.topPort; auto current = top;
    for (auto instance : probe.path) {
      auto result = dyn_cast<OpResult>(driver(current, value));
      require(result && result.getOwner() == instance.getOperation(), "route reads the wrong instance");
      require(instance->getAttrOfType<StringAttr>("example.metadata").getValue() == "keep",
              "instance metadata lost");
      current = named(circuit, instance.getModuleName());
      value = current.getBodyBlock()->getArgument(result.getResultNumber());
    }
    require(current == probe.module && driver(current, value) == probe.value,
            "probe route changed source SSA identity");
  }
  require(top.getPortName(5) != "ila_right_child_signed" &&
          top.getPortName(1) == "ila_right_child_signed" &&
          named(circuit, "Leaf").getPortName(1) != "ila_signed",
          "existing local names were not reserved");
  require(oldUse.getInput().getDefiningOp<InstanceOp>().getName() == "right",
          "original instance result use not remapped");
  auto unused = *named(circuit, "Unused").getBodyBlock()->getOps<InstanceOp>().begin();
  require(unused.getNumResults() == 5 &&
          unused->getAttrOfType<StringAttr>("example.metadata").getValue() == "keep",
          "unreachable parent instance signature/metadata not rebuilt");
  require(succeeded(verify(*root)), "invalid wired FIRRTL IR");
}
void rejection(MLIRContext &context) {
  auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<goldengate::WiredILAProbe> outputs(1); std::string error;
  for (auto target : {"Top.Unused.u", "Top.Top.missing", "~Top|Top/right:Mid>a"}) {
    SmallVector<Attribute> bad(raw.begin(), raw.end());
    bad.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr(target))}));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(bad)); auto before = dump(root.get());
    require(failed(goldengate::wireAutoILAProbesToTop(circuit, outputs, error)) &&
            outputs.size() == 1 && dump(root.get()) == before, "non-atomic invalid selection: " + error);
  }
  circuit->setAttr("rawAnnotations", raw);
  auto unused = *named(circuit, "Unused").getBodyBlock()->getOps<InstanceOp>().begin();
  SmallVector<std::pair<unsigned, PortInfo>> added{{1,
      PortInfo(b.getStringAttr("bad"), UIntType::get(&context, 1), Direction::Out)}};
  auto malformed = unused.cloneAndInsertPorts(added);
  unused.getResult(0).replaceAllUsesWith(malformed.getResult(0)); unused.erase();
  auto before = dump(root.get());
  require(failed(goldengate::wireAutoILAProbesToTop(circuit, outputs, error)) &&
          StringRef(error).contains("incompatible instance") && outputs.size() == 1 &&
          dump(root.get()) == before, "incompatible use mutated circuit: " + error);
  circuit->setAttr("rawAnnotations", b.getArrayAttr({})); before = dump(root.get());
  require(succeeded(goldengate::wireAutoILAProbesToTop(circuit, outputs, error)) &&
          outputs.size() == 1 && dump(root.get()) == before, "empty selection changed circuit");
}
void ambiguousNames(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top() {
        %first = firrtl.instance a_b @L(out c: !firrtl.uint<3>)
        %second = firrtl.instance a @M(out b_c: !firrtl.uint<5>)
        %ila_a_b_c = firrtl.wire : !firrtl.uint<1>
      }
      firrtl.module private @L(out %c: !firrtl.uint<3>) {}
      firrtl.module private @M(out %b_c: !firrtl.uint<5>) {}
      firrtl.module private @ila_wrapper() {}
      firrtl.module private @ila_firesim() {}
    }
  })mlir", &context);
  require(bool(root), "ambiguous name fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin(); OpBuilder b(&context);
  SmallVector<Attribute> annotations;
  for (auto target : {"Top.L.c", "Top.M.b_c"})
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr(target))}));
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  SmallVector<goldengate::WiredILAProbe> outputs; std::string error;
  require(succeeded(goldengate::wireAutoILAProbesToTop(circuit, outputs, error)), error);
  require(outputs.size() == 2 && outputs[0].source.suggestedName == "ila_a_b_c" &&
          outputs[1].source.suggestedName == "ila_a_b_c" &&
          outputs[0].topTarget != outputs[1].topTarget, "flattened paths aliased top ports");
  auto top = named(circuit, "Top");
  for (auto &output : outputs) {
    auto instance = output.source.path.front();
    require(driver(top, output.topPort) == instance.getResult(1) &&
            output.topPort.getType() == output.source.value.getType(),
            "flattened path collision aliased the source/type");
  }
  require(succeeded(verify(*root)), "invalid collision routing IR");
  SmallVector<Value> drivers;
  SmallVector<std::string> names;
  for (auto &output : outputs) {
    drivers.push_back(driver(top, output.topPort));
    names.push_back(top.getPortName(cast<BlockArgument>(output.topPort).getArgNumber()).str());
  }
  InstanceOp wrapper;
  require(succeeded(goldengate::attachAutoILAWrapper(circuit, outputs, {"collision"}, wrapper, error)), error);
  require(top.getNumPorts() == 0 && wrapper.getModuleName() != "ila_wrapper",
          "wrapper module namespace/interface collision");
  for (unsigned i = 0; i < outputs.size(); ++i)
    require(wrapper.getPortName(i + 1) == names[i] && driver(top, wrapper.getResult(i + 1)) == drivers[i],
            "wrapper aliased ambiguous flattened paths");
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto dict = cast<DictionaryAttr>(attr);
    if (dict.getAs<StringAttr>("class").getValue() == "firrtl.transforms.BlackBoxInlineAnno") {
      auto text = dict.getAs<StringAttr>("text").getValue();
      require(text.contains(".probe0 (" + names[0] + ")") && text.contains(".probe1 (" + names[1] + ")") &&
              !text.contains("  ila_firesim CL_FIRESIM_DEBUG"), "collateral lost allocated probe/IP names");
    }
  }
  require(succeeded(verify(*root)), "invalid collision wrapper IR");
}
void wrapper(MLIRContext &context) {
  auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top"); OpBuilder b(&context);
  top.insertPorts({{2, PortInfo(b.getStringAttr("clock"), ClockType::get(&context), Direction::In)}});
  auto clock = top.getBodyBlock()->getArgument(2);
  b.setInsertionPointToStart(top.getBodyBlock());
  b.create<WireOp>(top.getLoc(), UIntType::get(&context, 1), "ila_wrapper_inst");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  auto source = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::HostClockSource)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top>clock"))});
  annotations.push_back(source);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  SmallVector<goldengate::WiredILAProbe> routes; std::string error;
  require(succeeded(goldengate::wireAutoILAProbesToTop(circuit, routes, error)), error);
  SmallVector<Value> drivers;
  SmallVector<std::string> portNames;
  for (auto &route : routes) {
    drivers.push_back(driver(top, route.topPort));
    portNames.push_back(top.getPortName(cast<BlockArgument>(route.topPort).getArgNumber()).str());
  }
  auto before = dump(root.get()); InstanceOp instance;
  goldengate::ILAWrapperOptions options{"test-output", 2048, 4};
  auto invalidOptions = options; invalidOptions.dataDepth = 0;
  require(failed(goldengate::attachAutoILAWrapper(circuit, routes, invalidOptions, instance, error)) &&
          !instance && dump(root.get()) == before, "invalid IP parameters mutated circuit");
  auto invalidRoutes = routes; std::swap(invalidRoutes[0], invalidRoutes[1]);
  require(failed(goldengate::attachAutoILAWrapper(circuit, invalidRoutes, options, instance, error)) &&
          !instance && dump(root.get()) == before, "out-of-order routes mutated circuit");
  require(succeeded(goldengate::attachAutoILAWrapper(circuit, routes, options, instance, error)), error);
  require(top.getNumPorts() == 3 && top.getPortName(2) == "clock" &&
          top.getBodyBlock()->getArgument(2) == clock, "old top interface/SSA identity lost");
  require(instance.getName() != "ila_wrapper_inst" && instance.getNumResults() == 11,
          "wrapper instance namespace collision or probe count");
  auto external = *circuit.getOps<FExtModuleOp>().begin();
  require(external.getDefname() == external.getName() && external.getNumPorts() == 11 &&
          isa<ClockType>(external.getPortType(0)), "wrapper black box signature");
  for (unsigned i = 0; i < drivers.size(); ++i)
    require(instance.getPortName(i + 1) == portNames[i] &&
            instance.getPortDirection(i + 1) == Direction::In &&
            instance.getResult(i + 1).getType() == drivers[i].getType() &&
            driver(top, instance.getResult(i + 1)) == drivers[i], "wrapper probe identity/order/type changed");
  bool tcl = false, verilog = false, sink = false;
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto dict = cast<DictionaryAttr>(attr); auto cls = dict.getAs<StringAttr>("class").getValue();
    require(cls != goldengate::AnnotationClasses::InternalFpgaDebug, "private debug annotation not consumed");
    if (cls == "midas.stage.GoldenGateOutputFileAnnotation") {
      auto body = dict.getAs<StringAttr>("body").getValue();
      tcl = body.contains("CONFIG.C_NUM_OF_PROBES {10}") && body.contains("CONFIG.C_DATA_DEPTH {2048}") &&
            body.contains("CONFIG.ALL_PROBE_SAME_MU_CNT {4}") && body.contains("CONFIG.C_PROBE3_WIDTH {7}");
    }
    if (cls == "firrtl.transforms.BlackBoxInlineAnno") {
      auto body = dict.getAs<StringAttr>("text").getValue();
      verilog = dict.getAs<StringAttr>("name").getValue() == "test-output.ila_wrapper_inst.v" &&
          body.contains("`ifdef SYNTHESIS") && body.contains(".probe3 (" + portNames[3] + ")") &&
          body.contains("input [6:0] " + portNames[3]);
    }
    sink |= cls == goldengate::AnnotationClasses::HostClockSink;
  }
  require(tcl && verilog && sink, "ILA collateral or Clock sink missing");
  unsigned wired = 0;
  require(succeeded(goldengate::wireHostClock(circuit, wired, error, true)), error);
  require(wired == 1 && driver(top, instance.getResult(0)) == clock,
          "wrapper does not sample host clock");
  auto finalAnnotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  require(finalAnnotations[0] == source && finalAnnotations.size() == 4,
          "host source not retained or annotation preservation/cleanup failed");
  require(succeeded(verify(*root)), "invalid wrapper IR");
}
void hostPhase(MLIRContext &context) {
  OpBuilder b(&context);
  {
    auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin();
    auto privateClass = b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug);
    auto publicClass = b.getStringAttr(goldengate::AnnotationClasses::FpgaDebug);
    auto anno = [&](StringAttr cls, StringRef target) { return b.getDictionaryAttr({
        b.getNamedAttr("class", cls), b.getNamedAttr("target", b.getStringAttr(target))}); };
    auto keep = anno(b.getStringAttr("example.Keep"), "Top.Leaf.a");
    circuit->setAttr("rawAnnotations", b.getArrayAttr({anno(publicClass, "Top.Leaf.a"),
        anno(privateClass, "~Top|Leaf>signed"), keep}));
    std::string error;
    require(succeeded(goldengate::prepareAutoILAAnnotations(circuit, true, {}, error)), error);
    circuit.setNameAttr(b.getStringAttr("HostShim"));
    require(succeeded(goldengate::prepareAutoILAAnnotations(circuit, false, "Top", error)), error);
    require(circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr({
        anno(privateClass, "HostShim.Leaf.a"), anno(privateClass, "~HostShim|Leaf>signed"), keep}),
        "public conversion or circuit carry changed module/reference/unrelated identities");
    require(succeeded(goldengate::prepareAutoILAAnnotations(circuit, false, "Foreign", error)),
        "already carried identity should be accepted");
    circuit->setAttr("rawAnnotations", b.getArrayAttr({anno(privateClass, "Other.Leaf.a"), keep}));
    auto before = dump(root.get());
    require(failed(goldengate::prepareAutoILAAnnotations(circuit, false, "Top", error)) &&
        dump(root.get()) == before, "foreign debug target was silently carried or partially rewritten");
  }
  for (bool enabled : {false, true}) {
    auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin();
    auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
    SmallVector<Attribute> annotations;
    if (!enabled) annotations.append(raw.begin(), raw.end() - 1);
    // Disabled private targets need not be live (Scala also skips resolution).
    if (!enabled) annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::InternalFpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr("Top.Missing.notLive"))}));
    for (auto cls : {"firrtl.transforms.TopWiring.TopWiringAnnotation",
                     "firrtl.transforms.TopWiring.TopWiringOutputFilesAnnotation"})
      annotations.push_back(b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls))}));
    auto keep = raw[raw.size() - 1]; annotations.push_back(keep);
    auto publicDebug = b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::FpgaDebug)),
        b.getNamedAttr("target", b.getStringAttr("~Top|Top>a"))});
    annotations.push_back(publicDebug);
    circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
    auto body = dump(named(circuit, "Top"));
    unsigned probes = 99; std::string error;
    require(succeeded(goldengate::runAutoILA(circuit, enabled, {}, probes, error)), error);
    require(probes == 0 && dump(named(circuit, "Top")) == body &&
            circuit->getAttrOfType<ArrayAttr>("rawAnnotations") == b.getArrayAttr({keep, publicDebug}) &&
            circuit.getOps<FExtModuleOp>().empty(), "disabled/no-selection cleanup changed hardware or unrelated annotations");
  }
  auto root = fixture(context); auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top");
  top.insertPorts({{2, PortInfo(b.getStringAttr("clock"), ClockType::get(&context), Direction::In)}});
  SmallVector<Attribute> annotations(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  auto source = b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(goldengate::AnnotationClasses::HostClockSource)),
      b.getNamedAttr("target", b.getStringAttr("~Top|Top>clock"))});
  annotations.push_back(source); circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  unsigned probes = 99; std::string error;
  auto before = dump(root.get());
  require(failed(goldengate::runAutoILA(circuit, true, {"../bad", 1024, 2}, probes, error)) &&
          probes == 99 && dump(root.get()) == before, "invalid phase options changed hierarchy");
  require(succeeded(goldengate::runAutoILA(circuit, true, {"host-output", 4096, 3}, probes, error)), error);
  require(probes == 10 && top.getNumPorts() == 3, "host phase did not restore top interface");
  unsigned clocks = 99;
  require(succeeded(goldengate::wireHostClock(circuit, clocks, error)), error);
  require(clocks == 0, "final phase rewired already wired ILA clock");
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto cls = cast<DictionaryAttr>(attr).getAs<StringAttr>("class").getValue();
    require(cls != goldengate::AnnotationClasses::HostClockSource &&
            cls != goldengate::AnnotationClasses::HostClockSink &&
            cls != goldengate::AnnotationClasses::InternalFpgaDebug, "final host annotations were not consumed");
  }
  require(succeeded(verify(*root)), "invalid complete host AutoILA phase");
}
} // namespace
int main() {
  MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { routing(context); rejection(context); ambiguousNames(context); wrapper(context); hostPhase(context); }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  llvm::outs() << "AutoILA hierarchy routes, shared uses, name collisions and atomic rejection passed\n";
  return 0;
}
