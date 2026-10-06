// See LICENSE for license details.
#pragma once

#include "goldengate/ModelAnalysis.h"
#include <string>
#include <vector>

namespace goldengate {
// Requires: ground model ports and resolved FIRRTL when/last-connect semantics.
// No annotations consumed/produced, IR mutation or analyses invalidated.
// Reads FIRRTL field identities and hierarchy; rebuild after structural rewrites.
// Unnormalized conditional modules or multiply driven combinational fields
// produce explicit blockers, never an apparently complete dependency set.
// A partial version of CheckCombLoops' port connectivity. An unresolved
// output must never be treated as having no input dependencies: black boxes
// and unsupported operations require further analysis. The known output-only
// Rocket plusarg_reader is a configuration source with no target input paths.
// Memory read data is recognized by field identity, including static nested
// selections. Async readers follow address/enable; sync reads break the path.
// Async readwrite ports and new read-under-write remain explicit blockers.
// Dynamic vector reads follow the index and the same selected field of each
// reachable element. Literal indices follow only that element; out-of-range
// literal and zero-length selections produce blockers until normalized.
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
