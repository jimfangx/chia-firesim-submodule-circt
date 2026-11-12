// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Infer clocks for unclocked inter-model channels from top-level clock
// connections. This is the CIRCT counterpart of FindDefaultClocks.
// Also record uniquely proven ancillary hub-input aliases on the hub definition
// before ChannelExcision splits physical clock connections. FAMEModelGraph
// consumes these records to route every alias consumer through its declared
// domain's token-controlled gate and remove the redundant scalar input ABI.
// Distinct/unknown or multiply matched domains never acquire alias proofs.
mlir::LogicalResult findDefaultClocks(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
} // namespace goldengate
