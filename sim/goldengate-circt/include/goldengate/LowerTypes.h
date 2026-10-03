// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
// Resolve CHIRRTL and infer widths/resets before lowering aggregate operations.
// Transfer aggregate port
// targets in retained annotations to the corresponding ground ports.
mlir::LogicalResult lowerTypesWithRetainedTargets(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit,
    std::string &error);
} // namespace goldengate
