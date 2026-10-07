// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Map ordered rational ClockBridge lanes into the active simulator wrapper.
// The legacy GGSingleClockBridge symbol is preserved for collateral consumers.
// Includes the 32-bit decoded MCR snapshot bank; Nasti control-bus transport is
// a subsequent SimulationMapping step.
mlir::LogicalResult addClockBridge(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
} // namespace goldengate
