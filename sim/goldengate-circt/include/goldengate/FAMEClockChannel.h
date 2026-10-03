// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// SimWrapper.genClockChannel: expose a Decoupled vector of Boolean clock
// tokens, preserving the target's Clock payload and its ready/valid handshake.
mlir::LogicalResult addFAMEClockChannel(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
} // namespace goldengate
