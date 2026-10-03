// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Infer clocks for unclocked inter-model channels from top-level clock
// connections. This is the CIRCT counterpart of FindDefaultClocks.
mlir::LogicalResult findDefaultClocks(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
} // namespace goldengate
