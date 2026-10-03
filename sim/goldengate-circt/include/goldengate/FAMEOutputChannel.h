// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEPortAnalysis.h"
#include <string>

namespace goldengate {
// Replace scalar or grouped model outputs and their top-level connections with the
// decoupled source port used by FAME1OutputChannel.
// Wrapper ground-port DontTouch annotations and inner symbols move to the
// payload field ID; symbol names/visibility and InnerRefs stay unchanged.
// Unsupported attached metadata is rejected before the channel is changed.
// Callers must refresh port analyses and inner symbol tables after rewriting.
mlir::LogicalResult rewriteFAMEOutputChannel(const TopHierarchy &hierarchy,
                                              const FAMETopChannelPort &channel,
                                              std::string &error);
} // namespace goldengate
