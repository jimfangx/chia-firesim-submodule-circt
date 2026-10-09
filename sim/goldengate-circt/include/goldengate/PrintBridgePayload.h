// See LICENSE for license details.
#pragma once
#include "goldengate/CPUStreamRead.h"
#include "goldengate/CPUStreamCountBank.h"
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
// Program the stream wrapper with the same six-word bank and Pulsify state.
mlir::LogicalResult materializePrintBridgeStreamConfigs(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> streams,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Shared local transport helper for a freshly materialized stream bank.
mlir::LogicalResult materializePrintBridgeStreamAXI(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp config,
    unsigned addressBits, unsigned idBits,
    llvm::StringRef wrapperName, llvm::StringRef adapterName,
    circt::firrtl::FModuleOp &host, std::string &error);
// Compose one control, width adapter, register bank, local Nasti transport and
// outgoing CPU queue. Stage the complete batch before committing IR. Global
// stream allocation and driver collateral remain separate; retain annotations.
mlir::LogicalResult materializePrintBridgeHosts(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> controls,
    unsigned addressBits, unsigned idBits,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Buffer each local PrintBridge host using the shared CPUManagedStreamEngine
// 6144x512 synchronous queue. Retain circuit/annotation identities and all host
// ports; append streamCount UInt<13>. Stream index/address allocation is pending.
// Reject invalid or repeated identities atomically across the complete batch.
mlir::LogicalResult materializePrintBridgeHostQueues(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
// Bind queued hosts to the active post-FAME top's latency-zero output tokens,
// using BridgeIO.channelMapping and resolved CIRCT port/field identities.
// Preserve constructor/host order, platform host reset versus token reset,
// and HostPortIO's per-channel DecoupledHelper predicates. Consume the token
// ports; expose each host's local AXI control, buffered stream and live count.
// Global control/stream allocation and Print driver collateral are later stages.
// Validate/stage the complete batch before committing; empty batches are no-ops.
mlir::LogicalResult bindPrintBridgeHosts(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Consume bound Print queue outputs into native CPU AXI AR/R and read-only
// occupancy MCR words. Caller-supplied streams precede Print hosts; Print order
// comes from the binding registry, independently of module traversal. Validate
// each Print's implemented six-word MCR bank before allocation. Stage both
// transport and count binding so a failure leaves the entire circuit unchanged.
// Global MMIO dispatch, CPU write transport and driver assembly follow later.
mlir::LogicalResult mapPrintBridgeCPUStreams(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamSourcePort> precedingSources,
    llvm::ArrayRef<CPUStreamCountPort> precedingCounts, std::string &error);
// Recorded Rocket platform: derive its live TracerV payload/count pair, then
// append the bound Print hosts to the shared outgoing DMA and MCR allocation.
mlir::LogicalResult mapPrintBridgeRocketCPUStreams(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Allocate implemented Print configuration banks and the live CPU count bank
// through native MMIO AW/W/AR dispatch. Input: GGCPUStreamCountWrapper.
// Stage and verify the whole composition; response arbitration and driver
// address collateral remain separate boundaries. Fixed U250 Nasti widths.
mlir::LogicalResult mapPrintBridgeControlDispatch(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume every selected bank's B/R response through the native NastiRouter
// arbiters and 64-slot ID trackers. Input: GGControlReadDispatchWrapper.
// Stage all four passes; MMIO master bundle and driver assembly follow later.
mlir::LogicalResult mapPrintBridgeControlResponses(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Full Rocket allocation with instantiated Print hosts: consume the seven
// early Rocket banks and all Print B/R responses. Re-derive region bounds
// from live bank/DMA registries before composing; the four later Rocket
// banks retain explicit request/response ports for subsequent attachment.
mlir::LogicalResult mapPrintBridgeRocketControlResponses(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Attach the standalone Master.scala bank and consume its AW/W/AR/B/R at
// the live expanded Rocket allocation. Input: GGControlWriteTrackerWrapper.
// Revalidate all region identities/bounds before staging the bank, adapter,
// and binding together. The remaining TSI/BlockDev/FASED ports stay exposed.
mlir::LogicalResult mapPrintBridgeRocketSimulationMaster(
    circt::firrtl::CircuitOp circuit, std::string &error);

// Compose the live Rocket TSI scheduler, word queues, MMIO bank and allocated
// control slave after the expanded Print/SimulationMaster boundary. Atomic.
mlir::LogicalResult mapPrintBridgeRocketTSI(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Compose all nine live Rocket BlockDev channels, four queues, timing, and
// allocated MMIO slave after the expanded Print/TSI boundary. Atomic.
mlir::LogicalResult mapPrintBridgeRocketBlockDev(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
