// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include <string>
namespace goldengate {
// Generate allocation-parameterized synthesized_prints_t factories from bound
// queued hosts. Record offsets come from the same layout used to pack RTL.
// MMIO addresses, widget numbers and CPU stream indices must be supplied by
// later platform allocation. Validate the complete batch before changing text.
mlir::LogicalResult preparePrintBridgeDecoderHeader(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &header, std::string &error);
// Emit the selected Print/count Widget.genConstructor boundary after native
// MMIO master assembly. Resolve addresses and stream indices from the live
// allocation, validate the complete collection, and preserve IR/text on error.
// The fragment uses print-bridge-decoders.h and the standard GET_* guards.
mlir::LogicalResult preparePrintBridgeAllocatedHeader(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &header, std::string &error);
}
