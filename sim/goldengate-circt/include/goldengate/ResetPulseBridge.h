// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Materialize ResetPulseBridge tokens and its two-word decoded MCR bank.
mlir::LogicalResult addResetPulseBridge(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
} // namespace goldengate
