// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult assembleCPUStreamPort(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
/// Select the original memory platform or expanded Print B-buffer boundary.
/// Validate its exact scalar forwarding before generating the shared AXI adapter.
mlir::LogicalResult attachCPUStreamPort(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
}
