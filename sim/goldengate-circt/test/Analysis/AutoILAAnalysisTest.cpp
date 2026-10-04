// See LICENSE for license details.
#include "goldengate/AutoILAAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %a: !firrtl.uint<1>, in %clock: !firrtl.clock,
                        in %zero: !firrtl.uint<0>, in %unknown: !firrtl.uint,
                        in %bundle: !firrtl.bundle<x: uint<1>>) {
        %b = firrtl.wire : !firrtl.uint<8>
        %right_a = firrtl.instance right @Mid(out a: !firrtl.uint<1>)
        %left_a = firrtl.instance left @Mid(out a: !firrtl.uint<1>)
      }
      firrtl.module private @Mid(out %a: !firrtl.uint<1>) {
        %child_a = firrtl.instance child @Leaf(out a: !firrtl.uint<1>)
      }
      firrtl.module private @Leaf(out %a: !firrtl.uint<1>) {
        %signed = firrtl.wire : !firrtl.sint<7>
        %node = firrtl.node %a : !firrtl.uint<1>
      }
      firrtl.module private @Unused(in %u: !firrtl.uint<3>) {}
      firrtl.extmodule private @External(in u: !firrtl.uint<3>)
    }
  })mlir", &context);
  require(bool(root), "AutoILA fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  OpBuilder b(&context);
  auto annotation = [&](StringRef target,
                        StringRef klass = goldengate::AnnotationClasses::InternalFpgaDebug) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(klass)),
                               b.getNamedAttr("target", b.getStringAttr(target))});
  };
  SmallVector<Attribute> annotations{
      annotation("Top.Leaf.a"), annotation("Top.Top.a"),
      annotation("Top.Leaf.node"), annotation("Top.Top.b"),
      annotation("Top.Leaf.signed"), annotation("~Top|Leaf>signed"),
      annotation("Top.Mid.a"),
      annotation("does.not.exist", goldengate::AnnotationClasses::FpgaDebug)};
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  auto before = dump(*root);
  SmallVector<goldengate::AutoILAProbe> probes;
  std::string error;
  require(succeeded(goldengate::analyzeAutoILAProbes(circuit, probes, error)), error);
  require(dump(*root) == before, "AutoILA analysis mutated IR/annotations");
  const char *names[] = {"ila_b", "ila_a", "ila_right_a",
      "ila_right_child_signed", "ila_right_child_node", "ila_right_child_a",
      "ila_left_a", "ila_left_child_signed", "ila_left_child_node", "ila_left_child_a"};
  require(probes.size() == 10, "probe fanout/duplicate selection changed");
  for (unsigned i = 0; i < probes.size(); ++i) {
    require(probes[i].index == i && probes[i].suggestedName == names[i],
            "probe declaration/port/child ordering changed at " + std::to_string(i));
    require(bool(probes[i].value) && bool(probes[i].module), "probe lost SSA identity");
  }
  require(probes[0].width == 8 && !probes[0].isPort && probes[1].isPort,
          "local probe type/origin changed");
  require(probes[3].width == 7 && probes[3].path.size() == 2 &&
          probes[3].path[0].getName() == "right" &&
          probes[3].path[1].getName() == "child" &&
          probes[3].value == probes[7].value,
          "reused module path/SSA identity changed");
  // All rejected cases preserve prior results and annotations.
  for (auto target : {"Top.Unused.u", "Top.Top.missing", "Top.Leaf.a.x",
                      "Wrong.Top.a", "~Top|Top/right:Mid>a", "Top.Top.clock",
                      "Top.Top.zero", "Top.Top.unknown", "Top.Top.bundle",
                      "Top.External.u"}) {
    auto bad = annotations;
    bad.push_back(annotation(target));
    circuit->setAttr("rawAnnotations", b.getArrayAttr(bad));
    auto invalidBefore = dump(*root);
    require(failed(goldengate::analyzeAutoILAProbes(circuit, probes, error)),
            "accepted invalid AutoILA target " + std::string(target));
    require(probes.size() == 10 && dump(*root) == invalidBefore,
            "failed AutoILA analysis changed caller state");
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr({}));
  require(succeeded(goldengate::analyzeAutoILAProbes(circuit, probes, error)) &&
          probes.size() == 10, "empty selection changed existing outputs");
}
} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try { run(context); }
  catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
  llvm::outs() << "AutoILA probe SSA identity, ordering, fanout and atomic rejection passed\n";
  return 0;
}
