// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "goldengate/ControlAddressDecode.h"
#include "llvm/ADT/SmallVector.h"
#include <string>
namespace goldengate {
// Native AssertBridge: two token queue, lowest-ID priority, global-reset mask,
// enable/resume MMIO and accepted-token local cycle count. Atomic on failure.
mlir::LogicalResult materializeAssertionBridgeHosts(circt::firrtl::CircuitOp, unsigned, unsigned, llvm::SmallVectorImpl<circt::firrtl::FModuleOp>&, std::string&);
mlir::LogicalResult bindAssertionBridgeHosts(circt::firrtl::CircuitOp, llvm::ArrayRef<circt::firrtl::FModuleOp>, std::string&);
mlir::LogicalResult discoverBoundAssertionBridgeHosts(circt::firrtl::CircuitOp, llvm::SmallVectorImpl<circt::firrtl::FModuleOp>&, std::string&);
mlir::LogicalResult deriveAssertionControlMMIOCatalog(circt::firrtl::CircuitOp, llvm::ArrayRef<circt::firrtl::FModuleOp>, llvm::SmallVectorImpl<ControlMMIOWidget>&, std::string&);
mlir::LogicalResult prepareAssertionBridgeHeader(circt::firrtl::CircuitOp, llvm::ArrayRef<circt::firrtl::FModuleOp>, llvm::ArrayRef<ControlMMIORegion>, std::string&, std::string&);
}
