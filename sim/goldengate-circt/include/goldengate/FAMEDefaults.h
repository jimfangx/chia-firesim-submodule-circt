// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Label each top-level model and each direct, non-clock model-to-model
// connection, following the SFC FAMEDefaults boundary.
mlir::LogicalResult addFAMEDefaults(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
} // namespace goldengate
