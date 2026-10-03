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
#include <map>
#include <system_error>

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
  // A self connect contributes two SSA uses of the same driver operation.
  b.create<ConnectOp>(top.getLoc(), internalSink.getResult(), internalSink.getResult());
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
    require(failed(goldengate::wireGlobalReset(circuit, count, error)), "accepted invalid contract");
    require(count == 0 && dump(root.get()) == before && StringRef(error).contains(expected),
            "failed preflight mutated IR or gave wrong diagnostic: " + error);
  };
  reject({source, source}, "multiple"); // also invalid with no sinks
  reject({source, annotation(A::GlobalResetSink, "~Top|Child>sink")}, "no instance path");
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
    require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count == 0,
            "optional side failed: " + error);
    require(dump(top) == body && circuit->getAttr("rawAnnotations") == b.getArrayAttr({unrelated}),
            "optional side changed hardware or retained annotation");
  }
  set({unrelated, source, sink, annotation(A::PublicGlobalResetSink, "~Top|Top>second"),
       annotation(A::GlobalResetSink, "~Top|Top>internalSink"), sink});
  require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count == 3,
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
  require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count == 0 &&
          dump(root.get()) == completed, "repeated reset wiring changed circuit");
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>())
    if (module.getName() == name) return module;
  throw std::runtime_error("missing module " + name.str());
}
DictionaryAttr annotation(OpBuilder &b, StringRef cls, StringRef target) {
  return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls)),
                             b.getNamedAttr("target", b.getStringAttr(target))});
}
Value driver(FModuleOp module, Value dest) {
  Value result;
  for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>())
    if (connect.getDest() == dest) {
      require(!result, "multiple drivers"); result = connect.getSrc();
    }
  require(bool(result), "missing driver");
  return result;
}
void hierarchy(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %condition: !firrtl.uint<1>, out %result: !firrtl.uint<1>) {}
      firrtl.module @Mid(out %result: !firrtl.uint<1>) {}
      firrtl.module @Leaf(out %sink: !firrtl.uint<1>) {}
      firrtl.module @Unused() {}
    }
  })mlir", &context);
  require(bool(root), "hierarchy parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top"), mid = named(circuit, "Mid"), leaf = named(circuit, "Leaf");
  OpBuilder b(&context);
  auto type = UIntType::get(&context, 1);
  b.setInsertionPointToEnd(mid.getBodyBlock());
  // Route modules may themselves contain sinks. This collision must retain
  // the existing wire and allocate one input with a distinct name.
  auto midSink = b.create<WireOp>(mid.getLoc(), type, b.getStringAttr("InternalGlobalResetCondition"));
  auto leaf0 = b.create<InstanceOp>(mid.getLoc(), leaf, "leaf0");
  auto leaf1 = b.create<InstanceOp>(mid.getLoc(), leaf, "leaf1");
  leaf0->setAttr("example.metadata", b.getStringAttr("keep"));
  b.create<StrictConnectOp>(mid.getLoc(), mid.getBodyBlock()->getArgument(0), leaf0.getResult(0));
  b.setInsertionPointToEnd(top.getBodyBlock());
  auto mid0 = b.create<InstanceOp>(top.getLoc(), mid, "mid0");
  b.create<InstanceOp>(top.getLoc(), mid, "mid1");
  b.create<InstanceOp>(top.getLoc(), leaf, "direct");
  auto existing = b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(1), mid0.getResult(0));
  auto source = annotation(b, A::PublicGlobalResetSource, "~Top|Top>condition");
  auto sink = annotation(b, A::GlobalResetSink, "~Top|Leaf>sink");
  auto midAnnotation = annotation(b, A::GlobalResetSink, "~Top|Mid>InternalGlobalResetCondition");
  auto set = [&](ArrayRef<Attribute> attrs) { circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs)); };
  unsigned count; std::string error;
  // Changing a shared child signature would leave this unreachable parent's
  // reset input undriven. Reject the complete plan before editing anything.
  auto unused = named(circuit, "Unused");
  b.setInsertionPointToEnd(unused.getBodyBlock());
  auto orphan = b.create<InstanceOp>(unused.getLoc(), leaf, "orphan");
  set({source, sink}); auto before = dump(root.get());
  require(failed(goldengate::wireGlobalReset(circuit, count, error)) &&
          StringRef(error).contains("unreachable") && dump(root.get()) == before,
          "unreachable use did not reject atomically: " + error);
  orphan.erase();
  set({annotation(b, A::GlobalResetSource, "~Top|Mid>InternalGlobalResetCondition"), sink});
  before = dump(root.get());
  require(failed(goldengate::wireGlobalReset(circuit, count, error)) &&
          StringRef(error).contains("circuit-top source") && dump(root.get()) == before,
          "nested source did not reject atomically");
  set({source, sink, midAnnotation, sink});
  require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count == 2,
          "shared reset routing failed: " + error);
  require(succeeded(verify(*root)), "invalid shared reset IR");
  require(top.getNumPorts() == 2 && mid.getNumPorts() == 2 && leaf.getNumPorts() == 2 &&
          unused.getNumPorts() == 0, "route inputs duplicated or leaked to unrelated module");
  require(mid.getPortName(1) == "InternalGlobalResetCondition_0" &&
          leaf.getPortName(1) == "InternalGlobalResetCondition", "wrong sink route names");
  require(driver(mid, midSink.getResult()) == mid.getBodyBlock()->getArgument(1) &&
          driver(leaf, leaf.getBodyBlock()->getArgument(0)) == leaf.getBodyBlock()->getArgument(1),
          "sink reset does not come from route input");
  unsigned edges = 0;
  for (auto module : {top, mid}) {
    auto expected = module.getBodyBlock()->getArgument(module == top ? 0 : 1);
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      ++edges;
      require(instance.getNumResults() == 2 && driver(module, instance.getResult(1)) == expected,
              "a shared instance did not receive reset");
      if (instance.getName() == "leaf0")
        require(instance->getAttr("example.metadata") == b.getStringAttr("keep"), "instance metadata lost");
      if (instance.getName() == "mid0")
        require(existing.getSrc() == instance.getResult(0), "old instance uses were not replaced");
    }
  }
  require(edges == 5 && circuit->getAttr("rawAnnotations") == b.getArrayAttr({}), "missing shared routes");
  auto completed = dump(root.get());
  require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count == 0 &&
          dump(root.get()) == completed, "repeat hierarchy wiring changed IR");
}

// The immutable Rocket fixture has a source but no descendant debug sinks.
// Exercise its actual hierarchy using dedicated test-added wires; these are
// explicitly augmented inputs, not an enabled-debug SFC output oracle.
void goldenHierarchy(MLIRContext &context, StringRef input, StringRef output) {
  auto root = parseSourceFile<ModuleOp>(input, &context);
  require(bool(root), "golden candidate parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, circuit.getName());
  OpBuilder b(&context);
  SmallVector<Attribute> attrs(circuit->getAttrOfType<ArrayAttr>("rawAnnotations").getValue());
  std::map<Operation *, unsigned> oldPorts;
  for (auto module : circuit.getOps<FModuleOp>()) oldPorts[module] = module.getNumPorts();
  for (auto name : {StringRef("CSRFile"), StringRef("CLINT")}) {
    auto module = named(circuit, name);
    b.setInsertionPointToEnd(module.getBodyBlock());
    b.create<WireOp>(module.getLoc(), UIntType::get(&context, 1), b.getStringAttr("iteration309ResetSink"));
    attrs.push_back(annotation(b, A::GlobalResetSink,
        "~" + circuit.getName().str() + "|" + name.str() + ">iteration309ResetSink"));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs));
  unsigned count; std::string error;
  require(succeeded(goldengate::wireGlobalReset(circuit, count, error)) && count >= 2,
          "Rocket hierarchy wiring failed: " + error);
  require(succeeded(verify(*root)), "invalid augmented Rocket IR");
  Value source;
  for (auto wire : top.getBodyBlock()->getOps<WireOp>())
    if (wire.getName() == "condition") source = wire.getResult();
  require(bool(source), "immutable source condition missing");
  unsigned modules = 0, edges = 0;
  for (auto module : circuit.getOps<FModuleOp>()) {
    auto n = oldPorts.at(module);
    if (module.getNumPorts() != n) {
      ++modules;
      require(module.getNumPorts() == n + 1 && module.getPortDirection(n) == Direction::In &&
              module.getBodyBlock()->getArgument(n).getType() == UIntType::get(&context, 1),
              "Rocket route signature mismatch");
      auto expected = module.getName() == "CSRFile" || module.getName() == "CLINT" ?
          "InternalGlobalResetCondition" : "condition";
      require(module.getPortName(n) == expected, "Rocket route name disagrees with Scala");
    }
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      FModuleOp child;
      for (auto candidate : circuit.getOps<FModuleOp>())
        if (candidate.getName() == instance.getModuleName()) child = candidate;
      if (!child) continue; // Extmodules have no reset sinks.
      auto old = oldPorts.at(child);
      if (child.getNumPorts() == old) continue;
      ++edges;
      require(driver(module, instance.getResult(old)) ==
          (module == top ? source : module.getBodyBlock()->getArgument(n)), "Rocket reset path broken");
    }
  }
  require(modules == 9 && edges == 9, "immutable CSRFile/CLINT hierarchy routing changed");
  std::error_code ec;
  llvm::raw_fd_ostream out(output, ec);
  require(!ec, "cannot save augmented candidate"); root->print(out);
  llvm::outs() << "Rocket augmented reset hierarchy PASS: " << modules << " modules, " << edges
               << " instance edges, " << count << " sinks\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    MLIRContext context;
    context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    run(context, false); run(context, true); hierarchy(context);
    if (argc == 3) goldenHierarchy(context, argv[1], argv[2]);
    else require(argc == 1, "expected input.mlir output.mlir or no arguments");
    llvm::outs() << "Global reset wiring PASS\n";
    return 0;
  } catch (const std::exception &error) {
    llvm::errs() << error.what() << '\n'; return 1;
  }
}
