// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/CombDependencyAnalysis.h"
#include <optional>

namespace goldengate {
// Module-local channel identity after data port channelization. Clock aliases
// must already resolve to the original model input clock. None is legal only
// for a VirtualClockChannel model, matching SFC genMetadata(None).
struct FAMEModelControlChannel {
  std::string name;
  circt::firrtl::Direction direction;
  std::optional<std::string> modelClockName;
};

// One complete transformed definition, not one instance. The caller captures
// local dependencies before port channelization and obtains clocks from the hub
// or virtual clock constructors afterwards. Their Values must belong to model.
// Empty clockChannel denotes VirtualClockChannel and requires one constant-one
// raw clock control. A real clock channel may contain multiple ordered domains.
struct FAMEModelControlPlan {
  circt::firrtl::FModuleOp model;
  std::string clockChannel;
  llvm::SmallVector<FAMEHubClockControl, 0> clocks;
  llvm::SmallVector<FAMEModelControlChannel, 0> channels;
  llvm::SmallVector<LocalChannelDependency, 0> dependencies;
};

// Compose complete FAME-1 channel control for ordered, distinct definitions.
// Validate the entire list's module ownership, clock/control identities, full
// channel coverage, dependencies, handshake connects and existing fired state
// before mutation. Then build fired registers and the output-valid, input-ready,
// fired-next-state and finishing equations using each definition's own plan.
// Shared instances remain independent: no target state, ports, instances or
// wiring are merged or rewritten here. Host/clock controls and data channels
// must already exist; annotations and XDC are preserved. Operation/state scans
// become stale, while module/port and hierarchy analyses remain valid.
mlir::LogicalResult rewriteFAMEModelControls(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<FAMEModelControlPlan> plans, std::string &error);
} // namespace goldengate
