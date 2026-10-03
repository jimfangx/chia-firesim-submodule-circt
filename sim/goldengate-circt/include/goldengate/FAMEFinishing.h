// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>

namespace goldengate {
// Complete a target cycle when every input token is valid, every output has
// fired or can fire now, and the target clock token is valid. The clock token
// becomes ready when all data channels can advance. Existing FIRRTL connects
// for the finishing wire and clock ready field are replaced in place.
// Annotations consumed/produced: none.
mlir::LogicalResult rewriteFAMEFinishing(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<std::string> inputChannels,
    llvm::ArrayRef<std::string> outputChannels,
    llvm::StringRef clockChannel, std::string &error);
} // namespace goldengate
