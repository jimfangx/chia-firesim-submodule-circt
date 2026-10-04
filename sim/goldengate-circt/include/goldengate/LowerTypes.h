// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
enum class RetainedTargetScope { All, FpgaDebugOnly };
// Requires: imported FIRRTL with rawAnnotations and resolvable local selectors.
// Consumes: DontTouch/host/global reset signal/FPGA debug references to empty
// aggregates; temporary leaf identities and identical expanded signal/debug
// annotations.
// Produces: ground DontTouch/FAME host clock/reset and HostClockSource/Sink
// targets; public/internal FPGA debug ComponentName leaves (preserving legacy
// JSON spelling when supplied); public/internal GlobalResetCondition source
// and sink leaves (the wiring consumer enforces one source); exact
// AutoCounter/trigger event, clock and
// optional reset references for public and internal classes; exact FAME channel
// clock/source/sink
// references in original order, including
// DecoupledForwardChannel optional nested ready/valid references, and exact
// FAMEChannelPortsAnnotation optional clockPort and ordered ports references.
// Mutates: resolves CHIRRTL, infers widths/resets, lowers aggregates, uniquifies
// declaration names, synchronizes instance ports, and transfers retained targets.
// Requires analyses: FIRRTL subtype/field IDs and CIRCT inner symbol namespaces.
// Preserves: annotation payload/order, unrelated annotations, native leaf symbols
// and InnerRefs. Structural analyses must be rebuilt after lowering.
// Outputs: ground retained selectors, unique names, matching module/instance
// interfaces, and no temporary identities. AutoCounter/trigger selectors each
// name one ground value; their consumers check missing clock/reset metadata.
// Unresolved ground trigger/global reset references remain for unused-annotation
// cleanup when one side of the wiring is absent.
// Native aggregate inner symbols remain subject to CIRCT LowerTypes validation.
// FpgaDebugOnly is for late AutoILA lowering after FAME channel consumption:
// transfer debug selections only and preserve the other raw records verbatim,
// including historical channel endpoints whose ports have already been removed.
mlir::LogicalResult lowerTypesWithRetainedTargets(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit,
    std::string &error, RetainedTargetScope scope = RetainedTargetScope::All);
} // namespace goldengate
