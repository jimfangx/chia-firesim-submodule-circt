// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace goldengate {
// Required input invariants: the model has FAME decoupled input ports, a
// targetCycleFinishing wire, and a one-bit fired register per input channel.
// Resolve native fired state by goldengate.fameFiredChannel, with name-based
// compatibility for wholly untagged legacy models. Reject duplicate identities.
// Annotations consumed/produced: none.
// IR mutations: drive input ready with finishing & !fired, creating the
// field connect when the channel was just introduced.
mlir::LogicalResult rewriteFAMEInputReadies(
    circt::firrtl::FModuleOp module, llvm::ArrayRef<std::string> inputChannels,
    std::string &error);
} // namespace goldengate
