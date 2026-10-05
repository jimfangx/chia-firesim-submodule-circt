// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace goldengate {
// Materialize PrintBridge.scala's combinational PrintRecordBag payload from
// retained BridgeIO constructors. Constructor order controls both packed bits
// and goldengate.printPayload offsets. Host integration/ROI/streams remain pending;
// annotations are retained. Validate all constructors before modifying IR.
mlir::LogicalResult materializePrintBridgePayloads(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);

// Create the accepted-cycle staging/idle encoding boundary for each payload.
// Inputs are the payload's valid/data outputs and explicit host controls; ROI
// register programming and DMA width adaptation remain outside this boundary.
// Matches Queue(1, pipe=true, flow=false). Retains annotations and validates
// every payload/identity before adding any module.
mlir::LogicalResult materializePrintBridgeTokenStages(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> payloads,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
} // namespace goldengate
