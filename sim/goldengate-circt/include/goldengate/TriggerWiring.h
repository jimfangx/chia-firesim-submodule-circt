// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Consume unused trigger annotations, or emit Scala-compatible local accounting
// for distinct credit/debit sources and node sinks on the circuit top base clock,
// including unconditional local wire/node aliases of the same input Clock port.
// Unsupported hardware cases fail before mutation.
mlir::LogicalResult wireTriggers(
    circt::firrtl::CircuitOp circuit, unsigned &consumed,
    std::string &error);
} // namespace goldengate
