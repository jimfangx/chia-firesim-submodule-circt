// See LICENSE for license details.
#pragma once
#include "goldengate/ChannelAnalysis.h"
#include "mlir/IR/Builders.h"

namespace goldengate {
struct RationalClockSchedule {
  llvm::SmallVector<unsigned> periods;
  unsigned counterWidth;
};

// FindScaledPeriodGCD semantics, in clockInfo order. Reject invalid Scala Int
// ratios and schedules exceeding ClockBridge.scala's 16-bit countdown limit.
std::optional<RationalClockSchedule> analyzeRationalClockSchedule(
    llvm::ArrayRef<RationalClockInfo> clocks, std::string &error);

// Emit always-valid, ordered edge bits into an existing producer body. Inputs
// are its host Clock, synchronous UInt<1> reset and UInt<1> downstream ready.
// The schedule must come from analyzeRationalClockSchedule. Advance only on
// ready; reset all countdowns to zero. The caller connects valid to one.
llvm::SmallVector<mlir::Value> buildRationalClockTokens(
    mlir::OpBuilder &builder, mlir::Location loc, mlir::Value clock,
    mlir::Value reset, mlir::Value ready, const RationalClockSchedule &schedule);
} // namespace goldengate
