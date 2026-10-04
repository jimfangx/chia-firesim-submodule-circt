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

struct ILAWrapperOptions {
  std::string outputBaseFilename;
  unsigned dataDepth = 1024;
  unsigned probeTriggers = 2;
};
// Complete the hardware/collateral boundary after top wiring: replace the
// temporary top outputs by wrapper instance inputs, restore the old interface,
// emit inline Verilog and IP-generation annotations, and mark the Clock sink.
// The caller subsequently runs wireHostClock with retainSource=true. Routes
// must be the complete appended port suffix in probe order; their topPort
// values are invalid after success. Reject malformed routes before mutation.
mlir::LogicalResult attachAutoILAWrapper(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<WiredILAProbe> routes,
    const ILAWrapperOptions &options, circt::firrtl::InstanceOp &wrapper,
    std::string &error);
} // namespace goldengate
