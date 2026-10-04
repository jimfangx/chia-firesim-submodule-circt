// See LICENSE for license details.
#include "goldengate/HostClockWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <stdexcept>
using namespace mlir;
using namespace circt::firrtl;
using A = goldengate::AnnotationClasses;
namespace {
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
std::string dump(Operation *op) {
  std::string text; llvm::raw_string_ostream out(text); op->print(out); return text;
}
FModuleOp named(CircuitOp circuit, StringRef name) {
  for (auto module : circuit.getOps<FModuleOp>()) if (module.getName() == name) return module;
  throw std::runtime_error("missing module " + name.str());
}
DictionaryAttr annotation(OpBuilder &b, StringRef cls, StringRef target) {
  return b.getDictionaryAttr({b.getNamedAttr("class", b.getStringAttr(cls)),
                             b.getNamedAttr("target", b.getStringAttr(target))});
}
Value driver(FModuleOp module, Value dest) {
  Value result;
  for (auto connect : module.getBodyBlock()->getOps<StrictConnectOp>())
    if (connect.getDest() == dest) { require(!result, "multiple drivers"); result = connect.getSrc(); }
  require(bool(result), "missing driver"); return result;
}
void local(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(in %clock: !firrtl.clock, out %sink: !firrtl.clock,
                        in %other: !firrtl.clock, out %bad: !firrtl.uint<1>) {}
    }
  })mlir", &context);
  require(bool(root), "parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin(); auto top = named(circuit, "Top");
  OpBuilder b(&context); b.setInsertionPointToEnd(top.getBodyBlock());
  auto internal = b.create<WireOp>(top.getLoc(), ClockType::get(&context), b.getStringAttr("internal"));
  b.create<ConnectOp>(top.getLoc(), internal.getResult(), top.getBodyBlock()->getArgument(2));
  b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(1), top.getBodyBlock()->getArgument(2));
  auto alias = b.create<NodeOp>(top.getLoc(), top.getBodyBlock()->getArgument(0), b.getStringAttr("clockAlias"));
  auto source = annotation(b, A::HostClockSource, "~Top|Top>clock");
  auto sink = annotation(b, A::HostClockSink, "~Top|Top>sink");
  auto keep = annotation(b, "example.Keep", "~Top|Top>sink");
  auto global = annotation(b, A::GlobalResetSource, "~Top|Top>bad");
  auto set = [&](ArrayRef<Attribute> attrs) { circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs)); };
  unsigned count; std::string error;
  auto reject = [&](ArrayRef<Attribute> attrs, StringRef expected) {
    set(attrs); auto before = dump(root.get());
    require(failed(goldengate::wireHostClock(circuit, count, error)) && count == 0 &&
            dump(root.get()) == before && StringRef(error).contains(expected), "preflight: " + error);
  };
  reject({keep}, "exactly one"); reject({sink}, "exactly one");
  reject({source, source}, "multiple");
  reject({source, annotation(b, A::HostClockSink, "~Top|Top>bad")}, "Clock");
  reject({annotation(b, A::HostClockSource, "~Top|Top>bad"), sink}, "source must be Clock");
  reject({source, annotation(b, A::HostClockSink, "~Top|Top>other")}, "outputs");
  reject({source, annotation(b, A::HostClockSink, "~Top|Top>missing")}, "local lowered");
  reject({source, annotation(b, A::HostClockSink, "~Top|Top>clockAlias")}, "internal sinks must be wires");
  reject({source, annotation(b, A::HostClockSink, "~Top|Top>clock")}, "outputs");
  // The unused source is deliberately unresolved, matching Scala's no-sink path.
  auto absent = annotation(b, A::HostClockSource, "~Top|Top>missing");
  auto body = dump(top);
  set({keep, absent, global});
  require(succeeded(goldengate::wireHostClock(circuit, count, error)) && count == 0 &&
          dump(top) == body && circuit->getAttr("rawAnnotations") == b.getArrayAttr({keep, global}),
          "no-sink execute: " + error);
  set({keep, absent, global});
  require(succeeded(goldengate::wireHostClock(circuit, count, error, true)) && count == 0 &&
          dump(top) == body && circuit->getAttr("rawAnnotations") == b.getArrayAttr({absent, keep, global}),
          "no-sink apply retention: " + error);
  set({keep, source, global, sink, annotation(b, A::HostClockSink, "~Top|Top>internal"), sink});
  require(succeeded(goldengate::wireHostClock(circuit, count, error, true)) && count == 2,
          "local apply: " + error);
  require(driver(top, internal.getResult()) == top.getBodyBlock()->getArgument(0) &&
          driver(top, top.getBodyBlock()->getArgument(1)) == top.getBodyBlock()->getArgument(0), "wrong local clock");
  require(circuit->getAttr("rawAnnotations") == b.getArrayAttr({source, keep, global}), "apply annotation order");
  require(succeeded(goldengate::wireHostClock(circuit, count, error)) && count == 0 &&
          circuit->getAttr("rawAnnotations") == b.getArrayAttr({keep, global}), "final consumption");
  set({annotation(b, A::HostClockSource, "~Top|Top>clockAlias"), sink});
  require(succeeded(goldengate::wireHostClock(circuit, count, error)) && count == 1 &&
          driver(top, top.getBodyBlock()->getArgument(1)) == alias.getResult() &&
          alias.getInput() == top.getBodyBlock()->getArgument(0), "node source identity: " + error);
  require(succeeded(verify(*root)), "invalid local IR");
}
void hierarchy(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" attributes {rawAnnotations = []} {
      firrtl.module @Top(out %sink: !firrtl.clock) {}
      firrtl.module @Source(in %clock: !firrtl.clock) {}
      firrtl.module @Sink(out %sink: !firrtl.clock) {}
    }
  })mlir", &context);
  require(bool(root), "hierarchy parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = named(circuit, "Top"), source = named(circuit, "Source"), sink = named(circuit, "Sink");
  OpBuilder b(&context); b.setInsertionPointToEnd(sink.getBodyBlock());
  b.create<WireOp>(sink.getLoc(), ClockType::get(&context), b.getStringAttr("HostClockSource"));
  b.setInsertionPointToEnd(top.getBodyBlock());
  b.create<InstanceOp>(top.getLoc(), source, "source");
  b.create<InstanceOp>(top.getLoc(), sink, "sink0"); b.create<InstanceOp>(top.getLoc(), sink, "sink1");
  auto attrs = b.getArrayAttr({annotation(b, A::HostClockSource, "~Top|Source>clock"),
                             annotation(b, A::HostClockSink, "~Top|Top>sink"),
                             annotation(b, A::HostClockSink, "~Top|Sink>sink")});
  circuit->setAttr("rawAnnotations", attrs); unsigned count; std::string error;
  auto extra = b.create<InstanceOp>(top.getLoc(), source, "ambiguous"); auto before = dump(root.get());
  require(failed(goldengate::wireHostClock(circuit, count, error)) && dump(root.get()) == before &&
          StringRef(error).contains("unique source instance"), "ambiguous source accepted"); extra.erase();
  require(succeeded(goldengate::wireHostClock(circuit, count, error)) && count == 2, "hierarchy: " + error);
  require(top.getNumPorts() == 1 && source.getNumPorts() == 2 && sink.getNumPorts() == 2 &&
          source.getPortDirection(1) == Direction::Out && source.getPortName(1) == "clock_0" &&
          sink.getPortDirection(1) == Direction::In && sink.getPortName(1) == "HostClockSource_0", "route signatures");
  require(driver(source, source.getBodyBlock()->getArgument(1)) == source.getBodyBlock()->getArgument(0), "upward route");
  Value routed;
  for (auto instance : top.getBodyBlock()->getOps<InstanceOp>())
    if (instance.getModuleName() == "Source") routed = instance.getResult(1);
  require(driver(top, top.getBodyBlock()->getArgument(0)) == routed, "LCA clock");
  for (auto instance : top.getBodyBlock()->getOps<InstanceOp>())
    if (instance.getModuleName() == "Sink") require(driver(top, instance.getResult(1)) == routed, "shared sink clock");
  require(driver(sink, sink.getBodyBlock()->getArgument(0)) == sink.getBodyBlock()->getArgument(1), "sink clock");
  require(succeeded(verify(*root)), "invalid hierarchy IR");
}
// Augment the imported immutable Rocket hierarchy with explicit Clock probes.
// These are transfer tests, not an assertion that the golden enables ILA sinks.
void golden(MLIRContext &context, StringRef input, StringRef output) {
  auto root = parseSourceFile<ModuleOp>(input, &context); require(bool(root), "golden parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin(); auto top = named(circuit, circuit.getName());
  OpBuilder b(&context); b.setInsertionPointToEnd(top.getBodyBlock());
  auto source = b.create<WireOp>(top.getLoc(), ClockType::get(&context), b.getStringAttr("iteration356Clock"));
  auto zero = b.create<ConstantOp>(top.getLoc(), UIntType::get(&context, 1), llvm::APInt(1, 0));
  auto clock = b.create<AsClockPrimOp>(top.getLoc(), zero.getResult());
  b.create<StrictConnectOp>(top.getLoc(), source.getResult(), clock.getResult());
  SmallVector<Attribute> attrs{annotation(b, A::HostClockSource, "~FireSim|FireSim>iteration356Clock")};
  std::map<Operation *, unsigned> oldPorts;
  for (auto module : circuit.getOps<FModuleOp>()) oldPorts[module] = module.getNumPorts();
  for (auto name : {StringRef("CSRFile"), StringRef("CLINT")}) {
    auto module = named(circuit, name); b.setInsertionPointToEnd(module.getBodyBlock());
    b.create<WireOp>(module.getLoc(), ClockType::get(&context), b.getStringAttr("iteration356Sink"));
    attrs.push_back(annotation(b, A::HostClockSink, "~FireSim|" + name.str() + ">iteration356Sink"));
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(attrs)); unsigned count; std::string error;
  require(succeeded(goldengate::wireHostClock(circuit, count, error)) && count == 2, "Rocket: " + error);
  unsigned modules = 0, edges = 0;
  for (auto module : circuit.getOps<FModuleOp>()) {
    auto n = oldPorts.at(module);
    if (module.getNumPorts() != n) {
      ++modules; require(module.getNumPorts() == n + 1 && module.getPortDirection(n) == Direction::In &&
        module.getBodyBlock()->getArgument(n).getType() == ClockType::get(&context), "Rocket route type");
      require(module.getPortName(n) == (module.getName() == "CSRFile" || module.getName() == "CLINT" ?
              "HostClockSource" : "iteration356Clock"), "Rocket route name");
    }
    for (auto instance : module.getBodyBlock()->getOps<InstanceOp>()) {
      FModuleOp child;
      for (auto candidate : circuit.getOps<FModuleOp>()) if (candidate.getName() == instance.getModuleName()) child = candidate;
      if (!child || child.getNumPorts() == oldPorts.at(child)) continue;
      ++edges; require(driver(module, instance.getResult(oldPorts.at(child))) ==
                       (module == top ? source.getResult() : module.getBodyBlock()->getArgument(n)), "Rocket route edge");
    }
  }
  require(modules == 9 && edges == 9 && succeeded(verify(*root)), "Rocket route hierarchy");
  std::error_code ec; llvm::raw_fd_ostream out(output, ec); require(!ec, "cannot save candidate"); root->print(out);
  llvm::outs() << "Rocket augmented host clock PASS: 9 module inputs, 9 instance edges, 2 sinks\n";
}
}
int main(int argc, char **argv) {
  try {
    MLIRContext context; context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
    local(context); hierarchy(context);
    if (argc == 3) golden(context, argv[1], argv[2]); else require(argc == 1, "expected input.mlir output.mlir");
    llvm::outs() << "Host clock wiring PASS\n"; return 0;
  } catch (const std::exception &error) { llvm::errs() << error.what() << '\n'; return 1; }
}
