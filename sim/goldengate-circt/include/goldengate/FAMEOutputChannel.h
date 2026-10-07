// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEPortAnalysis.h"
#include <string>

namespace goldengate {
// Resolve a grouped output alias to its payload leaf using branch annotations
// and ordered SSA producer connections. No IR mutation; shared aliases must
// belong to complete, compatible source branches.
std::optional<std::string> getFAMEOutputAliasField(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    unsigned modelPort, unsigned topPort, std::string &error);

// Replace scalar or grouped model outputs and their top-level connections with the
// decoupled source port used by FAME1OutputChannel.
// Wrapper ground-port DontTouch annotations and wrapper/model inner symbols
// move to payload field IDs; symbol names/visibility and InnerRefs stay unchanged.
// Model annotations must already be consumed; identities add no protection.
// Unsupported attached metadata is rejected before the channel is changed.
// Callers must refresh port analyses and inner symbol tables after rewriting.
mlir::LogicalResult rewriteFAMEOutputChannel(const TopHierarchy &hierarchy,
                                              const FAMETopChannelPort &channel,
                                              std::string &error);
} // namespace goldengate
