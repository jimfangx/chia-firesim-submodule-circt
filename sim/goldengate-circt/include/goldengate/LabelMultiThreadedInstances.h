// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
mlir::LogicalResult labelMultiThreadedInstances(
    circt::firrtl::CircuitOp circuit, bool enableMultiThreading,
    std::string &error);
} // namespace goldengate
