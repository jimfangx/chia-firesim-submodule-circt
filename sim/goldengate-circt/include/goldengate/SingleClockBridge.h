// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Map the supported single 1:1 ClockBridge into the active simulator wrapper.
// Includes the 32-bit decoded MCR snapshot bank; Nasti control-bus transport is
// a subsequent SimulationMapping step.
mlir::LogicalResult addSingleClockBridge(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
} // namespace goldengate
