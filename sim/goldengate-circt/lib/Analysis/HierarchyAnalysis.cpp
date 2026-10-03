// See LICENSE for license details.
#include "goldengate/HierarchyAnalysis.h"
#include "circt/Support/InstanceGraph.h"
#include "llvm/ADT/DenseMap.h"

using namespace circt;
using namespace circt::firrtl;

std::optional<goldengate::TopHierarchy>
goldengate::analyzeTopHierarchy(CircuitOp circuit, std::string &error) {
  igraph::InstanceGraph graph(circuit);
  auto *topNode = graph.lookup(mlir::StringAttr::get(circuit.getContext(),
                                                     circuit.getName()));
  if (!topNode || !topNode->noUses()) {
    error = "circuit top module is absent or instantiated elsewhere";
    return std::nullopt;
  }
  auto top = mlir::dyn_cast<FModuleOp>(topNode->getModule().getOperation());
  if (!top) {
    error = "circuit top is not an internal FIRRTL module";
    return std::nullopt;
  }

  llvm::DenseMap<mlir::Operation *, igraph::InstanceGraphNode *> children;
  for (auto *record : *topNode) {
    auto instance = record->getInstance<InstanceOp>();
    if (instance)
      children[instance.getOperation()] = record->getTarget();
  }

  TopHierarchy result{top, {}};
  llvm::DenseMap<unsigned, unsigned> seenTopPorts;
  top.walk([&](mlir::Operation *operation) {
    mlir::Value dest, src;
    if (auto connect = mlir::dyn_cast<StrictConnectOp>(operation)) {
      dest = connect.getDest();
      src = connect.getSrc();
    } else if (auto connect = mlir::dyn_cast<ConnectOp>(operation)) {
      dest = connect.getDest();
      src = connect.getSrc();
    } else {
      return;
    }
    auto destArg = mlir::dyn_cast<mlir::BlockArgument>(dest);
    auto srcArg = mlir::dyn_cast<mlir::BlockArgument>(src);
    auto destResult = mlir::dyn_cast<mlir::OpResult>(dest);
    auto srcResult = mlir::dyn_cast<mlir::OpResult>(src);
    mlir::BlockArgument topArg;
    mlir::OpResult instanceResult;
    if (destArg && srcResult) {
      topArg = destArg;
      instanceResult = srcResult;
    } else if (srcArg && destResult) {
      topArg = srcArg;
      instanceResult = destResult;
    } else {
      return;
    }
    if (topArg.getOwner() != top.getBodyBlock())
      return;
    auto instance = mlir::dyn_cast<InstanceOp>(instanceResult.getOwner());
    if (!instance || !children.count(instance.getOperation()))
      return;
    auto topPort = topArg.getArgNumber();
    auto instancePort = instanceResult.getResultNumber();
    if (seenTopPorts.count(topPort)) {
      error = "top port has more than one direct instance connection: " +
              top.getPortName(topPort).str();
      return;
    }
    auto child = children.lookup(instance.getOperation());
    auto childModule = child ? mlir::dyn_cast<FModuleLike>(
                                   child->getModule().getOperation()) : FModuleLike();
    if (!childModule || childModule.getModuleName() != instance.getModuleName() ||
        instancePort >= childModule.getPorts().size()) {
      error = "instance graph and child port identity disagree at " +
              top.getPortName(topPort).str();
      return;
    }
    seenTopPorts[topPort] = instancePort;
    result.connections.push_back({topPort, instance, instancePort});
  });
  if (!error.empty())
    return std::nullopt;
  return result;
}
