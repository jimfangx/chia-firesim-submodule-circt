// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEPortAnalysis.h"
#include <string>

namespace goldengate {
// Replace one scalar model output and its top-level connection with the
// decoupled source port used by FAME1OutputChannel.
mlir::LogicalResult rewriteFAMEOutputChannel(const TopHierarchy &hierarchy,
                                              const FAMETopChannelPort &channel,
                                              std::string &error);
} // namespace goldengate
