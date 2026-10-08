// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Promote labeled local model instances through every parent use until they
// reach the circuit main, honoring aggregate flips. Consume completed model
// labels; retain other annotations. `promoted` counts removed instances, not
// the number of top-level peers after fanout.
mlir::LogicalResult extractModels(circt::firrtl::CircuitOp circuit,
                                  unsigned &promoted, std::string &error);
}
