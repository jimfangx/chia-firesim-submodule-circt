// See LICENSE for license details.
#include "goldengate/PromotePassthroughConnections.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/Support/raw_ostream.h"
#include <stdexcept>

using namespace mlir;
using namespace circt::firrtl;
namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
std::string dump(Operation *op) {
  std::string text;
  llvm::raw_string_ostream out(text);
  op->print(out);
  return text;
}
void run(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top() {}
      firrtl.module @Child(in %clock: !firrtl.clock, out %out: !firrtl.uint<8>) {
        %reg = firrtl.reg %clock : !firrtl.clock, !firrtl.uint<8>
        firrtl.strictconnect %out, %reg : !firrtl.uint<8>
      }
      firrtl.module @Model(in %clock: !firrtl.clock,
          in %input: !firrtl.uint<8>, out %source: !firrtl.uint<8>,
          out %alias: !firrtl.uint<8>, out %signedSource: !firrtl.sint<13>,
          out %signedAlias: !firrtl.sint<13>, out %passthrough: !firrtl.uint<8>,
          out %expression: !firrtl.uint<8>, out %ambiguous: !firrtl.uint<8>,
          out %clockAlias: !firrtl.clock, out %wideAlias: !firrtl.uint<9>,
          out %cycleA: !firrtl.uint<8>, out %cycleB: !firrtl.uint<8>,
          out %hierarchySource: !firrtl.uint<8>, out %hierarchyAlias: !firrtl.uint<8>) {
        %primitive = firrtl.xor %input, %input : (!firrtl.uint<8>, !firrtl.uint<8>) -> !firrtl.uint<8>
        firrtl.strictconnect %source, %primitive : !firrtl.uint<8>
        %node = firrtl.node %source : !firrtl.uint<8>
        %wire = firrtl.wire : !firrtl.uint<8>
        firrtl.strictconnect %wire, %node : !firrtl.uint<8>
        firrtl.strictconnect %alias, %wire : !firrtl.uint<8>
        %signedReg = firrtl.reg %clock : !firrtl.clock, !firrtl.sint<13>
        firrtl.strictconnect %signedSource, %signedReg : !firrtl.sint<13>
        firrtl.strictconnect %signedAlias, %signedSource : !firrtl.sint<13>
        firrtl.strictconnect %signedReg, %signedAlias : !firrtl.sint<13>
        firrtl.strictconnect %passthrough, %input : !firrtl.uint<8>
        %changed = firrtl.xor %source, %input : (!firrtl.uint<8>, !firrtl.uint<8>) -> !firrtl.uint<8>
        firrtl.strictconnect %expression, %changed : !firrtl.uint<8>
        firrtl.strictconnect %ambiguous, %source : !firrtl.uint<8>
        firrtl.strictconnect %ambiguous, %input : !firrtl.uint<8>
        firrtl.strictconnect %clockAlias, %clock : !firrtl.clock
        firrtl.connect %wideAlias, %source : !firrtl.uint<9>, !firrtl.uint<8>
        firrtl.strictconnect %cycleA, %cycleB : !firrtl.uint<8>
        firrtl.strictconnect %cycleB, %cycleA : !firrtl.uint<8>
        %childClock, %childOut = firrtl.instance child @Child(in clock: !firrtl.clock, out out: !firrtl.uint<8>)
        firrtl.strictconnect %childClock, %clock : !firrtl.clock
        firrtl.strictconnect %hierarchySource, %childOut : !firrtl.uint<8>
        firrtl.strictconnect %hierarchyAlias, %hierarchySource : !firrtl.uint<8>
      }
    }
  })mlir", &context);
  require(bool(root), "alias fixture parse failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  FModuleOp top, model;
  circuit.walk([&](FModuleOp m) {
    if (m.getName() == "Top") top = m;
    if (m.getName() == "Model") model = m;
  });
  SmallVector<std::pair<unsigned, PortInfo>> ports;
  for (auto port : model.getPorts()) ports.emplace_back(0, port);
  top.insertPorts(ports);
  OpBuilder b(top.getBodyBlock(), top.getBodyBlock()->end());
  auto instance = b.create<InstanceOp>(top.getLoc(), model, "model");
  SmallVector<StrictConnectOp> connects;
  for (unsigned i = 0; i < top.getNumPorts(); ++i) {
    auto arg = top.getBodyBlock()->getArgument(i);
    auto result = instance.getResult(i);
    bool input = top.getPortDirection(i) == Direction::In;
    connects.push_back(b.create<StrictConnectOp>(top.getLoc(),
        input ? result : arg, input ? arg : result));
  }
  require(succeeded(verify(*root)), "alias fixture verification failed");
  auto before = dump(model);
  unsigned promoted = 0;
  std::string error;
  require(succeeded(goldengate::promotePassthroughConnections(circuit, promoted, error)),
          "promotion failed");
  require(promoted == 4, "wrong alias/passthrough promotion count");
  require(connects[3].getSrc() == instance.getResult(2), "node/wire alias kept duplicate source");
  require(connects[5].getSrc() == instance.getResult(4), "signed alias kept duplicate source");
  require(connects[6].getSrc() == top.getBodyBlock()->getArgument(1), "input passthrough lost");
  require(connects[14].getSrc() == instance.getResult(13), "hierarchical opaque source lost canonical output");
  for (unsigned i : {2u, 4u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
    require(connects[i].getSrc() == instance.getResult(i),
            "opaque expression, ambiguity, clock, width conversion or cycle was promoted");
  require(dump(model) == before, "promotion changed model hardware");
  require(succeeded(verify(*root)), "promoted IR verification failed");
}
void runLateInstance(MLIRContext &context) {
  auto root = parseSourceString<ModuleOp>(R"mlir(module {
    firrtl.circuit "Top" {
      firrtl.module @Top(out %out: !firrtl.uint<8>) {
        %a = firrtl.wire : !firrtl.uint<8>
        %b = firrtl.wire : !firrtl.uint<8>
        %sinkIn = firrtl.instance sink @Sink(in in: !firrtl.uint<8>)
        firrtl.strictconnect %sinkIn, %a : !firrtl.uint<8>
        firrtl.connect %out, %b : !firrtl.uint<8>, !firrtl.uint<8>
        %sourceOut = firrtl.instance source @Source(out out: !firrtl.uint<8>)
        firrtl.strictconnect %a, %sourceOut : !firrtl.uint<8>
        firrtl.strictconnect %b, %sourceOut : !firrtl.uint<8>
        %zero = firrtl.constant 0 : !firrtl.uint<8>
        firrtl.strictconnect %out, %zero : !firrtl.uint<8>
      }
      firrtl.module @Source(out %out: !firrtl.uint<8>) {
        %one = firrtl.constant 1 : !firrtl.uint<8>
        firrtl.strictconnect %out, %one : !firrtl.uint<8>
      }
      firrtl.module @Sink(in %in: !firrtl.uint<8>) {}
    }
  })mlir", &context);
  require(bool(root), "late-instance fixture parse failed");
  require(succeeded(verify(*root)), "late-instance fixture verification failed");
  auto circuit = *root->getOps<CircuitOp>().begin();
  auto top = *circuit.getOps<FModuleOp>().begin();
  SmallVector<Operation *> connections;
  top.walk([&](Operation *op) {
    if (isa<ConnectOp, StrictConnectOp>(op)) connections.push_back(op);
  });
  unsigned promoted = 0;
  std::string error;
  require(succeeded(goldengate::promotePassthroughConnections(circuit, promoted, error)),
          "late-instance promotion failed");
  require(promoted == 2, "late instance aliases were not promoted");
  require(succeeded(verify(*root)), "late instance source does not dominate promoted use");
  SmallVector<Operation *> after;
  top.walk([&](Operation *op) {
    if (isa<ConnectOp, StrictConnectOp>(op)) after.push_back(op);
  });
  require(after == connections, "promotion changed connection priority/order");
  for (unsigned i : {0u, 1u}) {
    auto source = connections[i]->getOperand(1).getDefiningOp<InstanceOp>();
    require(bool(source) && source.getName() == "source",
            "late instance source was not used directly");
    require(source->isBeforeInBlock(connections[i]),
            "late source declaration was not hoisted before its use");
  }
}

} // namespace
int main() {
  MLIRContext context;
  context.loadDialect<FIRRTLDialect, circt::hw::HWDialect>();
  try {
    run(context);
    runLateInstance(context);
    llvm::outs() << "PASS typed output aliases, opaque hierarchy, passthrough and unsafe-path boundaries\n";
  } catch (const std::exception &e) {
    llvm::errs() << e.what() << '\n';
    return 1;
  }
  return 0;
}
