// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>
#include <string>

namespace goldengate {
// A direct FAMETop port connection to a model instance port. The indices are
// stable within the imported FIRRTL operations and survive name collisions.
struct TopPortConnection {
  unsigned topPort;
  circt::firrtl::InstanceOp instance;
  unsigned instancePort;
};

struct TopHierarchy {
  circt::firrtl::FModuleOp top;
  llvm::SmallVector<TopPortConnection> connections;
};

// Mirror FAMEUtils.getTopConnects for direct FIRRTL connections. Later FAME
// passes can follow these typed operations instead of rebuilding SFC targets.
std::optional<TopHierarchy>
analyzeTopHierarchy(circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
