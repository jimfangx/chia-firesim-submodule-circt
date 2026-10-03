// See LICENSE for license details.
#include "goldengate/GlobalResetWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
using A = goldengate::AnnotationClasses;
namespace {
void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
void run(MLIRContext &context, bool publicSource) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %reset: !firrtl.uint<1>, out %sink: !firrtl.uint<1>,
                        out %second: !firrtl.uint<1>, out %wide: !firrtl.uint<8>) {}
      firrtl.module @Child(out %sink: !firrtl.uint<1>) {}
    }
  })mlir", &context);
  require(bool(root), "parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  OpBuilder b(&context); b.setInsertionPointToEnd(top.getBodyBlock());
  auto type = UIntType::get(&context, 1);
  auto condition = b.create<WireOp>(top.getLoc(), type, b.getStringAttr("condition"));
  auto inverted = b.create<NotPrimOp>(top.getLoc(), top.getBodyBlock()->getArgument(0));
  auto blocker = b.create<NotPrimOp>(top.getLoc(), inverted.getResult());
  auto sourceDriver = b.create<StrictConnectOp>(top.getLoc(), condition.getResult(), blocker.getResult());
  auto zero = b.create<ConstantOp>(top.getLoc(), type, llvm::APInt(1, 0));
  for (unsigned i = 1; i <= 2; ++i)
    b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(i), zero.getResult());
  auto internalSink = b.create<WireOp>(top.getLoc(), type, b.getStringAttr("internalSink"));
  b.create<ConnectOp>(top.getLoc(), internalSink.getResult(), zero.getResult());
  auto annotation = [&](StringRef cls, StringRef target) {
    return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls)),
                               b.getNamedAttr("target", b.getStringAttr(target))});
  };
  auto unrelated = annotation("example.Keep", "~Top|Top>sink");
  auto source = annotation(publicSource ? A::PublicGlobalResetSource : A::GlobalResetSource,
                           "~Top|Top>condition");
  auto sink = annotation(A::GlobalResetSink, "~Top|Top>sink");
  std::string error; unsigned count = 99;
  auto set = [&](ArrayRef<Attribute> attrs) {
    circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs));
  };
  auto reject = [&](ArrayRef<Attribute> attrs, StringRef expected) {
    set(attrs); auto before = dump(root.get());
    require(failed(goldengate::wireLocalGlobalReset(circuit, count, error)), "accepted invalid contract");
    require(count == 0 && dump(root.get()) == before && StringRef(error).contains(expected),
            "failed preflight mutated IR or gave wrong diagnostic: " + error);
  };
  reject({source, source}, "multiple"); // also invalid with no sinks
  reject({source, annotation(A::GlobalResetSink, "~Top|Child>sink")}, "one module");
  reject({source, annotation(A::GlobalResetSink, "~Top|Top>wide")}, "UInt<1>");
  reject({source, annotation(A::GlobalResetSink, "~Top|Top>reset")}, "outputs");
  reject({source, annotation(A::GlobalResetSink, "~Top|Top>missing")}, "local lowered declaration");
  reject({annotation(A::GlobalResetSource, "~Top|Top>wide"), sink}, "source must be UInt<1>");
  reject({source, annotation(A::GlobalResetSink, "~Top|Top>condition")}, "distinct");
  reject({source, b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(A::GlobalResetSink))})},
         "no target");
  // Optional source/sink rules consume even unresolved (but well-formed) targets.
  for (auto attrs : {b.getArrayAttr({unrelated, annotation(A::GlobalResetSink, "~Top|Top>missing")}),
                     b.getArrayAttr({unrelated, annotation(A::GlobalResetSource, "~Top|Top>missing")})}) {
    circuit->setAttr("rawAnnotations", attrs);
    auto body = dump(top);
    require(succeeded(goldengate::wireLocalGlobalReset(circuit, count, error)) && count == 0,
            "optional side failed: " + error);
    require(dump(top) == body && circuit->getAttr("rawAnnotations") == b.getArrayAttr({unrelated}),
            "optional side changed hardware or retained annotation");
  }
  set({unrelated, source, sink, annotation(A::PublicGlobalResetSink, "~Top|Top>second"),
       annotation(A::GlobalResetSink, "~Top|Top>internalSink"), sink});
  require(succeeded(goldengate::wireLocalGlobalReset(circuit, count, error)) && count == 3,
          "reset wiring failed: " + error);
  require(succeeded(verify(*root)), "invalid reset IR");
  require(circuit->getAttr("rawAnnotations") == b.getArrayAttr({unrelated}), "cleanup changed unrelated annotations");
  require(sourceDriver.getSrc() == blocker.getResult() && blocker.getInput() == inverted.getResult(),
          "source passthrough blocker changed");
  for (auto value : SmallVector<Value>{top.getBodyBlock()->getArgument(1), top.getBodyBlock()->getArgument(2),
                                      internalSink.getResult()}) {
    unsigned drivers = 0;
    for (auto connect : top.getBodyBlock()->getOps<StrictConnectOp>())
      if (connect.getDest() == value) {
        ++drivers; require(connect.getSrc() == condition.getResult(), "wrong reset driver");
      }
    require(drivers == 1, "reset sink has multiple or missing drivers");
    for (auto connect : top.getBodyBlock()->getOps<ConnectOp>())
      require(connect.getDest() != value, "placeholder connect survived");
  }
  auto completed = dump(root.get());
  require(succeeded(goldengate::wireLocalGlobalReset(circuit, count, error)) && count == 0 &&
          dump(root.get()) == completed, "repeated reset wiring changed circuit");
}
} // namespace
int main() {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context, false); run(context, true);
    llvm::outs() << "Global reset wiring PASS\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n'; return 1;
  }
}
