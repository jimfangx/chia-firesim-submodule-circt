// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// DefineBUFGCE + ReplaceAbstractClockGates from midas.passes.xilinx.
// Retarget existing instances in place: SSA values, connections, inner symbols,
// annotations and generated-clock metadata retain their identities.
// Unsupported gate schemas and symbol collisions fail before any mutation.
mlir::LogicalResult specializeXilinxClockGates(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
