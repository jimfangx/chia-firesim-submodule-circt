// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Scala ReadyValidChannel with two flow queues, a two-entry reference queue,
// and separate host forward/reverse completion state. Payloads are packed UInt.
mlir::LogicalResult addFAMEReadyValidChannel(circt::firrtl::CircuitOp circuit,
                                            unsigned payloadWidth,
                                            std::string &error);
// Resolve boundary pairs from annotations, recursively pack passive integer bundle leaves,
// normalize external Valid payloads like SimUtils.buildChannelType, and replace
// passthroughs in the existing pipe wrapper. Activation transfers leaf renames.
mlir::LogicalResult addFAMEBoundaryReadyValidChannels(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
