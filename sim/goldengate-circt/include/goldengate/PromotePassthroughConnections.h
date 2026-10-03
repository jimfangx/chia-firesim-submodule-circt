// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Move pure wire paths through model instances to the wrapper connections.
// Complex expressions and ambiguous drivers are left in their model modules.
mlir::LogicalResult promotePassthroughConnections(
    circt::firrtl::CircuitOp circuit, unsigned &promoted, std::string &error);
} // namespace goldengate
