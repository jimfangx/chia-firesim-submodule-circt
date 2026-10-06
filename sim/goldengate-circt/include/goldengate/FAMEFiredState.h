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
  // genMetadata(Some(clock)) inputs start fired until the first clock token.
  // genMetadata(None) under VirtualClockChannel starts every channel unfired.
  bool hasClockDomain = true;
};

// Add missing fired registers to a decoupled model. Clocked input channels
// reset fired, whereas outputs and virtual-clock channels reset unfired.
// Reuse only registers identified by goldengate.fameFiredChannel, checking the
// same host clock/reset and initial-value contract. Allocate collision-free
// names for new state and attach that identity. A self-connect is added
// for each new register; rewriteFAMEFiredStates replaces its source.
mlir::LogicalResult ensureFAMEFiredRegisters(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<FAMEFiredChannel> channels, std::string &error);

// Required input invariants: the model has decoupled channel ports, a
// targetCycleFinishing wire, and one host-clocked, one-bit fired register per
// channel. The caller supplies each channel's resolved clock-domain enable.
// Annotations consumed/produced: none. Generated registers carry the internal
// goldengate.fameFiredChannel identity; legacy untagged boundaries are readable.
// IR mutations: replace fired-register next-state connects with the FAME-1
// token state transition after checking the SFC reset value. Virtual-clock
// channels require a constant-one enable. Validate all channels before mutation.
mlir::LogicalResult rewriteFAMEFiredStates(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<FAMEFiredChannel> channels, std::string &error);
} // namespace goldengate
