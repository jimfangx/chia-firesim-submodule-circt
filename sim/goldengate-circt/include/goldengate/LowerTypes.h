// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
// Resolve CHIRRTL and infer widths/resets before lowering aggregate operations.
// Expand retained DontTouch port, wire, node, and register targets to all
// selected ground descendants, preserving fields and declaration order. Event
// selectors must select one ground value. Leaf inner symbols carry identity
// through expansion and namespace renames. Declaration names are unique,
// module/instance interfaces agree, and temporary identities are removed.
// Native aggregate inner symbols remain subject to CIRCT LowerTypes validation.
mlir::LogicalResult lowerTypesWithRetainedTargets(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit,
    std::string &error);
} // namespace goldengate
