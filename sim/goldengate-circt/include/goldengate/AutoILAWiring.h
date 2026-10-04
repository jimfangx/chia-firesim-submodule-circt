// See LICENSE for license details.
#pragma once
#include "goldengate/AutoILAAnalysis.h"

namespace goldengate {
struct WiredILAProbe {
  AutoILAProbe source;
  mlir::Value topPort;
  std::string topTarget;
};
// AutoILA's TopWiring step, after retained-target LowerTypes. Export one ground
// probe per instance path, with local declarations before ports and children.
// Expand shared modules once and rebuild every instance use, including unused
// parents. Allocate names per module and connect by SSA/port identity so name
// collisions cannot alias probes. Returned paths refer to live rebuilt instances.
// Debug annotations stay pending for wrapper/collateral construction. Invalid
// selections or hierarchy fail before mutation; append outputs only on success.
mlir::LogicalResult wireAutoILAProbesToTop(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<WiredILAProbe> &outputs, std::string &error);
} // namespace goldengate
