// See LICENSE for license details.
#pragma once

#include "goldengate/ModelAnalysis.h"
#include <optional>
#include <string>

namespace goldengate {
// The top-level channel interface that FAMETransform will create. These are
// typed model bindings until the point where a new FIRRTL port name is needed.
struct FAMETopChannelPort {
  const ModelChannelBinding *binding;
  std::string portName;
  circt::firrtl::BundleType type;
};

struct FAMEPortPlan {
  llvm::SmallVector<FAMETopChannelPort> sinks;
  llvm::SmallVector<FAMETopChannelPort> sources;
  llvm::SmallVector<unsigned> staleTopPorts;
};

std::optional<FAMEPortPlan>
analyzeFAMEPorts(const TopHierarchy &hierarchy,
                 llvm::ArrayRef<ModelChannelBinding> bindings,
                 llvm::ArrayRef<GGChannelConnection> channels,
                 llvm::ArrayRef<circt::firrtl::FModuleLike> transformedModules,
                 std::string &error);
} // namespace goldengate
