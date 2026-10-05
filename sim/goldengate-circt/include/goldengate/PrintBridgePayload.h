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

// Connect payload and token-stage instances with PrintBridge's accepted-cycle
// counter and inclusive unsigned ROI. The four configuration words and the
// already pulsified flush signal remain explicit inputs at this boundary.
// Validate all pairs before modifying IR; retain constructor annotations.
mlir::LogicalResult materializePrintBridgeControls(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> payloads,
    llvm::ArrayRef<circt::firrtl::FModuleOp> stages,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Six readable/writable MCR words and Pulsify state feed the ROI wrapper.
// This decoded register boundary does not yet allocate an AXI/stream region.
mlir::LogicalResult materializePrintBridgeConfigs(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> controls,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Local Nasti slave backed by the shared Lib.scala MCRFile transport. Batch
// preflight preserves target circuit and annotations; no global address/stream
// allocation occurs at this boundary.
mlir::LogicalResult materializePrintBridgeAXIControls(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> configs,
    unsigned addressBits, unsigned idBits,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Connect accepted-cycle controls to the 512-bit CPU stream boundary. Port
// PrintBridge's one-group MultiWidthFifo (not the separate CPU stream queue),
// with low slices first and flush-valid injection only for narrow tokens.
// Configuration/flush inputs remain explicit; annotations are retained.
mlir::LogicalResult materializePrintBridgeStreams(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> controls,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
} // namespace goldengate
