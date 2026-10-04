// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
// Requires: imported FIRRTL with rawAnnotations and resolvable local selectors.
// Consumes: DontTouch references to empty aggregates; temporary leaf identities.
// Produces: ground DontTouch targets and exact AutoCounter/trigger event,
// clock and optional reset references, for public and internal classes; exact
// TargetClockChannel/PipeChannel clock/source/sink references in original order.
// Mutates: resolves CHIRRTL, infers widths/resets, lowers aggregates, uniquifies
// declaration names, synchronizes instance ports, and transfers retained targets.
// Requires analyses: FIRRTL subtype/field IDs and CIRCT inner symbol namespaces.
// Preserves: annotation payload/order, unrelated annotations, native leaf symbols
// and InnerRefs. Structural analyses must be rebuilt after lowering.
// Outputs: ground retained selectors, unique names, matching module/instance
// interfaces, and no temporary identities. AutoCounter/trigger selectors each
// name one ground value; their consumers check missing clock/reset metadata.
// Unresolved ground trigger references remain for unused-annotation cleanup.
// Native aggregate inner symbols remain subject to CIRCT LowerTypes validation.
mlir::LogicalResult lowerTypesWithRetainedTargets(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit,
    std::string &error);
} // namespace goldengate
