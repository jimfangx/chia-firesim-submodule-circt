// See LICENSE for license details.
#include "goldengate/AutoILAWiring.h"
#include "goldengate/AnnotationClasses.h"
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
}
} // namespace
int main() {
  MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { routing(context); rejection(context); ambiguousNames(context); }
  catch (const std::exception &e) { llvm::errs() << e.what() << '\n'; return 1; }
  llvm::outs() << "AutoILA hierarchy routes, shared uses, name collisions and atomic rejection passed\n";
  return 0;
}
