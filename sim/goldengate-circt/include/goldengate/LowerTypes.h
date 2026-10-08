// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
enum class RetainedTargetScope { All, FpgaDebugOnly };
// Requires: imported FIRRTL with rawAnnotations and resolvable local selectors.
// Consumes: DontTouch/host/global reset signal/FPGA debug references to empty
// aggregates and zero-width leaves; temporary leaf identities and identical
// expanded signal/debug annotations.
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
// Exact zero-width references reject, matching SFC RTRenamer after RemoveZeroWidth.
// Fanout annotations omit zero-width leaves, including widths inferred as zero.
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
// Requires: fully initialized imported target FIRRTL and retained annotations.
// Runs the ground-target transfer above followed by CIRCT ExpandWhens, matching
// the Scala LowForm input to FAME. Conditional/last-connect drivers become
// explicit muxes before FAME hierarchy and data dependencies are analyzed.
// Consumes/produces annotations as above; ExpandWhens adds no GG annotations.
// Mutates: FIRRTL types, conditional regions and connections. Preserves named
// target identities/metadata; structural analyses must be rebuilt. No GG
// analyses required. Output: ground ports, no whens, one final driver per field.
mlir::LogicalResult normalizeFAMEInput(
    mlir::ModuleOp module, circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
