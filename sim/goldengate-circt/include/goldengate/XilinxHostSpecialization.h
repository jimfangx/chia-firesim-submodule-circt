// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// DefineBUFGCE + ReplaceAbstractClockGates from midas.passes.xilinx.
// Retarget existing instances in place: SSA values, connections, inner symbols,
// annotations and generated-clock metadata retain their identities.
// A later production stage may reuse this pass's marked, validated canonical
// BUFGCE declaration. Other symbol collisions and unsupported gate schemas fail
// before any mutation.
mlir::LogicalResult specializeXilinxClockGates(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
