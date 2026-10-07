// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Scala ReadyValidChannel with two flow queues, a two-entry reference queue,
// and separate host forward/reverse completion state. Payloads are packed UInt,
// including zero-bit payloads with queued valid tokens.
mlir::LogicalResult addFAMEReadyValidChannel(circt::firrtl::CircuitOp circuit,
                                            unsigned payloadWidth,
                                            std::string &error);
// Resolve boundary pairs from annotations, recursively pack annotation-selected passive integer bundle leaves,
// normalize external Valid payloads like SimUtils.buildChannelType, and replace
// passthroughs in the existing pipe wrapper. Excluded target input leaves are
// invalidated as in SimulationMapping; activation retains their metadata on the
// inner target and transfers selected leaf renames. Requires known-width passive
// integer payloads, at least one selected data leaf, an uninstantiated wrapper, and unique
// leaf endpoints including target-valid. Consumes no annotations; preserves the
// target hierarchy and records explicit wrapper target transfers.
mlir::LogicalResult addFAMEBoundaryReadyValidChannels(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
