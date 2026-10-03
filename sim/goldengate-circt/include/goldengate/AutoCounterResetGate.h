// See LICENSE for license details.
#pragma once
#include "goldengate/AutoCounterAnalysis.h"
#include "llvm/ADT/ArrayRef.h"

namespace goldengate {
// AutoCounterTransform::gateEventsWithReset, after event selection and
// LowerTypes, reset coercion, and ExpandWhens. Require known positive UInt
// event widths and synchronous UInt<1> resets. Retarget selected records to wires;
// preserve clock, reset, class, and payload for subsequent counter synthesis.
// All operands must be resolved in the current circuit. Invalid selections
// fail before either hardware or retained annotations are changed.
mlir::LogicalResult gateAutoCounterEventsWithReset(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<AutoCounterEvent> selected,
    std::string &error);
} // namespace goldengate
