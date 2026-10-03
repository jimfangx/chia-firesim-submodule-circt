// See LICENSE for license details.
#pragma once

#include "goldengate/ModelAnalysis.h"
#include <string>
#include <vector>

namespace goldengate {
// A partial version of CheckCombLoops' port connectivity. An unresolved
// output must never be treated as having no input dependencies: black boxes
// and unsupported operations require further analysis.
struct LocalChannelDependency {
  std::string outputChannel;
  std::vector<std::string> inputChannels;
  std::vector<std::string> unresolvedPorts;
  std::vector<std::string> unresolvedCauses;
};

std::vector<LocalChannelDependency> analyzeLocalChannelDependencies(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<ModelChannelBinding> bindings);
} // namespace goldengate
