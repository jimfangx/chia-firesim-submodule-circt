// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Trace clock-typed connections from annotated channel clock ports to the
// target clock channel, then attach the resulting clock domains to bridges.
mlir::LogicalResult analyzeChannelClocksAndUpdateBridges(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
