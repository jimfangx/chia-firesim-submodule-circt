// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace goldengate {
struct FAMEFiredChannel {
  std::string name;
  bool isInput;
  mlir::Value clockDomainEnable;
};

// Add missing fired registers to a decoupled model. Clocked input channels
// reset fired, whereas outputs reset unfired. Existing registers must obey the
// same host clock/reset and initial-value contract. A self-connect is added
// for each new register; rewriteFAMEFiredStates replaces its source.
mlir::LogicalResult ensureFAMEFiredRegisters(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<FAMEFiredChannel> channels, std::string &error);

// Required input invariants: the model has decoupled channel ports, a
// targetCycleFinishing wire, and one host-clocked, one-bit fired register per
// channel. The caller supplies each channel's resolved clock-domain enable.
// Annotations consumed/produced: none.
// IR mutations: replace fired-register next-state connects with the FAME-1
// token state transition, preserving the register's existing reset behavior.
mlir::LogicalResult rewriteFAMEFiredStates(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<FAMEFiredChannel> channels, std::string &error);
} // namespace goldengate
