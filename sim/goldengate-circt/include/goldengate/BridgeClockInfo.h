// See LICENSE for license details.
#pragma once
#include "goldengate/TargetUtils.h"

namespace goldengate {
// Check the analyzed rational domain of every mapped channel before its host
// module exists. BridgeIO.target is resolved separately after host binding.
mlir::LogicalResult validateBridgeChannelClocks(
    circt::firrtl::CircuitOp circuit, mlir::DictionaryAttr bridge,
    std::string &error);
// Resolve a retained BridgeIO to a CIRCT port and check every mapped channel
// against the sole ChannelClockInfo analysis. Never infer a default clock.
std::optional<GGTarget> resolveBridgeClockInfo(
    circt::firrtl::CircuitOp circuit, mlir::DictionaryAttr bridge,
    std::string &error);

// Preserve analyzed clock metadata when a bridge module has been imported into
// another assembly circuit. Modules retain their identity; only the circuit
// component of BridgeIO.target changes. Merge the selected channel entries into
// the destination map, preserving other domains and all unrelated annotations.
// Validate constructor, port type/direction and a unique live instance path
// before publishing any annotation. Both circuits must share an MLIR context.
mlir::LogicalResult importBridgeClockInfo(
    circt::firrtl::CircuitOp destination,
    circt::firrtl::CircuitOp source, llvm::StringRef widgetClass,
    std::string &error);
} // namespace goldengate
