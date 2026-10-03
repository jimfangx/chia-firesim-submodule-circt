// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Excise inter-model pipe/clock connections and bridge-sourced pipe fanouts
// at the FAME wrapper boundary.
mlir::LogicalResult exciseChannels(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
} // namespace goldengate
