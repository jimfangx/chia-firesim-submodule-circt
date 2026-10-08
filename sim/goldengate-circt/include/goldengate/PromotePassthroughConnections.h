// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Move pure wire paths through model instances to the wrapper connections.
// Resolve output-to-output aliases to their original model output, stopping
// at opaque hardware. Complex expressions and ambiguous drivers are left in
// their model modules; clock connections are not promoted.
mlir::LogicalResult promotePassthroughConnections(
    circt::firrtl::CircuitOp circuit, unsigned &promoted, std::string &error);
} // namespace goldengate
