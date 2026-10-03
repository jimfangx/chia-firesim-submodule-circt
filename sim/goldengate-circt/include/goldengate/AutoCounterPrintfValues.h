// See LICENSE for license details.
#pragma once
#include "goldengate/AutoCounterAnalysis.h"

namespace goldengate {
enum class AutoCounterMode { Accumulate, Identity };
struct AutoCounterPrintfValue {
  AutoCounterEvent source;
  AutoCounterMode mode;
  mlir::Value state, valueToPrint, printEnable;
};
// Port onModulePrintfImpl's value/state logic after selection, reset gating,
// LowerTypes and ExpandWhens. Accumulate prints the old UInt<64> count;
// Identity prints the current event and compares against the previous value.
// Trigger sinks and Print operations are synthesized by the next boundary.
// Preserve retained annotations. Invalid inputs fail without changing IR or
// appending results. Operands must resolve to the current annotation targets.
mlir::LogicalResult synthesizeAutoCounterPrintfValues(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<AutoCounterEvent> selected,
    llvm::SmallVectorImpl<AutoCounterPrintfValue> &values, std::string &error);
} // namespace goldengate
