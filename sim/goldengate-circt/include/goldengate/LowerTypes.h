// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
// Requires: imported FIRRTL with rawAnnotations and resolvable local selectors.
// Consumes: DontTouch references to empty aggregates; temporary leaf identities.
// Produces: ground DontTouch targets and exact AutoCounter target/clock/reset
// references, for both public and internal AutoCounter annotation classes.
// Mutates: resolves CHIRRTL, infers widths/resets, lowers aggregates, uniquifies
// declaration names, synchronizes instance ports, and transfers retained targets.
// Requires analyses: FIRRTL subtype/field IDs and CIRCT inner symbol namespaces.
// Preserves: annotation payload/order, unrelated annotations, native leaf symbols
// and InnerRefs. Structural analyses must be rebuilt after lowering.
// Outputs: ground retained selectors, unique names, matching module/instance
// interfaces, and no temporary identities. AutoCounter selectors each name one
// ground value; missing clock/reset metadata is checked by AutoCounter analysis.
// Native aggregate inner symbols remain subject to CIRCT LowerTypes validation.
mlir::LogicalResult lowerTypesWithRetainedTargets(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit,
    std::string &error);
} // namespace goldengate
