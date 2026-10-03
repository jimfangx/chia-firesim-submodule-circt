// See LICENSE for license details.
#pragma once
#include "goldengate/PrintStubs.h"

namespace goldengate {
struct WiredPrint {
  unsigned stubIndex;
  llvm::SmallVector<circt::firrtl::InstanceOp> instancePath;
  mlir::Value topPort;
  std::string absoluteSource, topTarget;
};
// BridgeTopWiring data step: add a passive output bundle through each instance
// path. Expand shared modules once and update every use with CIRCT port APIs.
// Keep input annotations pending until clock-source resolution and output
// annotation construction complete. Invalid selections fail before mutation.
mlir::LogicalResult wirePrintStubsToTop(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<PrintStub> stubs,
    llvm::SmallVectorImpl<WiredPrint> &outputs, std::string &error);
struct PrintClockSource {
  unsigned routeIndex;
  mlir::Value source;
  std::string sourceTarget;
};
// FindClockSources step of BridgeTopWiring, after LowerTypes/ExpandWhens.
// Resolve each local clock in its absolute instance context to one top input
// Clock. Analysis is read-only, including failure; append results only on success.
mlir::LogicalResult analyzePrintClockSources(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<PrintStub> stubs,
    llvm::ArrayRef<WiredPrint> routes,
    llvm::SmallVectorImpl<PrintClockSource> &sources, std::string &error);
} // namespace goldengate
