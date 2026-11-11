// See LICENSE for license details.
#include "goldengate/PromotePassthroughConnections.h"
#include "goldengate/PassthroughSourceAnalysis.h"
#include "llvm/ADT/SmallVector.h"

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::promotePassthroughConnections(
    CircuitOp circuit, unsigned &promoted, std::string &error) {
  promoted = 0;
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  if (!top) {
    error = "missing FAME wrapper top module";
    return failure();
  }
  goldengate::PassthroughSourceAnalysis tracer(circuit);
  auto rewrite = [&](Operation *op, Value dest, Value source) {
    if (isa<ClockType>(dest.getType()))
      return;
    bool isSink = false;
    if (auto arg = dyn_cast<BlockArgument>(dest))
      isSink = arg.getOwner() == top.getBodyBlock() &&
               top.getPortDirection(arg.getArgNumber()) == Direction::Out;
    if (auto instance = dest.getDefiningOp<InstanceOp>()) {
      auto port = cast<OpResult>(dest).getResultNumber();
      isSink = instance->getParentOfType<FModuleOp>() == top &&
               instance.getPortDirection(port) == Direction::In;
    }
    if (!isSink)
      return;
    auto root = tracer.trace(top, source);
    if (!root || *root == source || root->getType() != source.getType())
      return;
    if (auto *definition = root->getDefiningOp()) {
      if (definition->getBlock() != op->getBlock())
        return;
      if (!definition->isBeforeInBlock(op)) {
        // A wire may be declared and used before the instance driving it.
        // Bypassing the wire exposes that later instance result. Hoist only
        // operand-free instance declarations; moving connects would change
        // FIRRTL last-connect priority.
        if (!isa<InstanceOp>(definition) || definition->getNumOperands())
          return;
        definition->moveBefore(op);
      }
    }
    op->setOperand(1, *root);
    ++promoted;
  };
  // Snapshot connections before rewriting since promotion can move instance
  // declarations. Keep their original order, including mixed connect kinds.
  llvm::SmallVector<Operation *> connections;
  for (Operation &op : top.getBodyBlock()->getOperations())
    if (isa<ConnectOp, StrictConnectOp>(op))
      connections.push_back(&op);
  for (Operation *op : connections) {
    if (auto connect = dyn_cast<ConnectOp>(op))
      rewrite(op, connect.getDest(), connect.getSrc());
    else {
      auto strictConnect = cast<StrictConnectOp>(op);
      rewrite(op, strictConnect.getDest(), strictConnect.getSrc());
    }
  }
  return success();
}
