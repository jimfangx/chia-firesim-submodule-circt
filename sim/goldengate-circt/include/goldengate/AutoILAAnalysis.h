// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
struct AutoILAProbe {
  circt::firrtl::FModuleOp module;
  mlir::Value value;
  bool isPort;
  unsigned width;
  std::string target;
  std::string leafName;
  // Live CIRCT instances from the circuit top to the source module.
  llvm::SmallVector<circt::firrtl::InstanceOp> path;
  unsigned index;
  // TopWiring's suggested name, before routing namespace allocation.
  std::string suggestedName;
};

// Resolve internal debug annotations after retained-target LowerTypes.
// Match SFC TopWiring order: local declarations, local ports, child instances.
// This is the order at this IR boundary. SFC RemoveWires and CIRCT LowerTypes
// can produce different declaration orders from the same aggregate handoff;
// this analysis does not reorder circuit operations to hide that difference.
// Reused modules produce a probe for every instance path; repeated annotations
// on one value select that value once. No IR or annotations are changed, and
// failure leaves the caller's output untouched. Routing owns name allocation.
mlir::LogicalResult analyzeAutoILAProbes(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<AutoILAProbe> &probes, std::string &error);
} // namespace goldengate
