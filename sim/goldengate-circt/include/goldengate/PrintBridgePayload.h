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
// Consume the live TracerV FAME tokens at the bound Print boundary. Preserve
// Print hosts and analyzed clock metadata; expose native control/stream ports.
mlir::LogicalResult bindPrintBridgeTracerV(
    circt::firrtl::CircuitOp circuit, std::string &error);

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
// Attach the live TracerV trigger bank's native AXI transport before queue/CPU
// allocation. Reuse an imported adapter only if it equals the native hardware.
mlir::LogicalResult mapPrintBridgeTracerVControl(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Allocate implemented Print configuration banks and the live CPU count bank
// through native MMIO AW/W/AR dispatch. Input: GGCPUStreamCountWrapper.
// Stage and verify the whole composition; response arbitration and driver
// address collateral remain separate boundaries. Fixed U250 Nasti widths.
mlir::LogicalResult mapPrintBridgeControlDispatch(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume every selected Print/count bank and optional live TracerV bank's
// B/R response through the native NastiRouter
// arbiters and 64-slot ID trackers. Input: GGControlReadDispatchWrapper.
// Re-derive live register allocation and check connected AR slave identity.
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
// Bind eleven Rocket memory channels after the selected Print CPU AXI port.
// Preserve the live Print/optional TracerV allocation; stage and verify before
// publishing. Timing, ingress capacity and egress readiness remain explicit.
mlir::LogicalResult mapPrintBridgeFASEDTokens(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Attach host-reset-only saturating read/write counters after selected tokens.
// Preserve prior modules/SSA and revalidate the live allocated driver header.
mlir::LogicalResult mapPrintBridgeFASEDHostOutstanding(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Attach AW/W/AR ingress queues to selected host counters atomically.
// Qualified ingress reset clears pointers; raw dequeue/credit pulses remain
// explicit until host issue ordering is attached. Preserve live allocation/SSA.
mlir::LogicalResult mapPrintBridgeFASEDIngress(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Attach AW/W ingress credit counters after selected queues. Qualified ingress
// reset and accepted dequeue pulses drive the shared native helper; transaction
// ordering and host issue gates remain explicit. Publish only verified additions.
mlir::LogicalResult mapPrintBridgeFASEDIngressCredits(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Attach the native 20-entry read/write completion order after selected credits.
// Host issue dequeue readiness remains explicit; publish only verified additions.
mlir::LogicalResult mapPrintBridgeFASEDIngressOrder(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Consume selected order/queue boundaries into native strict/relaxed host issue.
// Actual AR/AW acceptance feeds outstanding counters; R/B handshakes remain
// explicit. Publish only verified additions and retain allocated host collateral.
mlir::LogicalResult mapPrintBridgeFASEDIngressIssue(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Append qualified host-clock AW/W enqueue assertions after selected issue.
// Atomic; retain existing port indices, module/ordinary SSA identities, raw
// annotations and allocated collateral while extending six internal instances.
mlir::LogicalResult mapPrintBridgeFASEDIngressDeadlock(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
// Attach synchronous sixteen-ID/eight-beat read response storage after selected
// ingress assertions. Preserve allocated hosts/SSA; scheduler/dequeue is explicit.
mlir::LogicalResult mapPrintBridgeFASEDReadBuffer(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);
/// Capture read requests and retire final response beats after selected storage.
/// Atomic; retain prior IR, allocated headers and consumed annotation targets.
mlir::LogicalResult mapPrintBridgeFASEDReadScheduler(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
mlir::LogicalResult mapPrintBridgeFASEDWriteEgress(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Close read/write target responses and native egress requests after selected
/// write scheduling. Atomic; preserve prior SSA and allocated identities.
mlir::LogicalResult mapPrintBridgeFASEDResponseReleaser(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Attach the targetFire-qualified model cycle and latency deadlines after
/// response release. Atomic; preserve prior SSA, allocation and copied targets.
mlir::LogicalResult mapPrintBridgeFASEDTimingCycle(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);

/// Attach the ten-entry, targetFire-qualified read deadline queue after model
/// cycle construction. Atomic; consume nextRead/deadline ports and retain SSA.
mlir::LogicalResult mapPrintBridgeFASEDReadLatency(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);

// Compose the eleven live FASED channels, host outstanding counters and three
// ingress queues after expanded Print/BlockDev. Atomic; egress readiness, host
// transactions and queue dequeue boundaries remain for later timing/MMIO passes.
mlir::LogicalResult mapPrintBridgeRocketFASEDIngress(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume the raw FASED dequeue/transaction boundaries into native credit,
// ordered issue and qualified deadlock checks. Input: completed expanded ingress.
// Atomic; retain existing MMIO identities and expose host request/response AXI.
mlir::LogicalResult mapPrintBridgeRocketFASEDIssue(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Buffer host R responses at the expanded Print/FASED issue boundary using
// ReadEgress's non-ROB per-ID queues. Atomic; qualified egress reset clears
// pointer/valid state, while RAM and delayed read ID remain unreset. The read
// address/dequeue and write response boundaries await subsequent schedulers.
mlir::LogicalResult mapPrintBridgeRocketFASEDReadBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume the read-buffer address/dequeue boundary through ReadEgress request
// state and token readiness, preserving the expanded Print MMIO allocation.
// Atomic; request/response payload and write egress await timing-model lowering.
mlir::LogicalResult mapPrintBridgeRocketFASEDReadScheduler(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume flat host B responses and write readiness through native WriteEgress
// acknowledgment state, retaining expanded Print banks and read scheduling.
// Atomic; timing-model write requests/responses remain explicit boundaries.
mlir::LogicalResult mapPrintBridgeRocketFASEDWriteEgress(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Close timing response and egress request boundaries with AXI4Releaser's
// one-entry replacement queues. Atomic; preserve expanded Print MMIO banks.
// Timing completion metadata and AW/W/AR readiness remain external.
mlir::LogicalResult mapPrintBridgeRocketFASEDResponseReleaser(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Add target-qualified 64-bit model cycle and runtime-latency release offsets.
// Atomic; preserve expanded Print MMIO banks and all response boundaries.
// Latency register attachment and completion queues remain subsequent stages.
mlir::LogicalResult mapPrintBridgeRocketFASEDTimingCycle(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume read completion metadata through the ten-entry flow-through latency
// queue. Atomic; preserve expanded Print banks and model-cycle boundaries.
// Write completion and latency register attachment remain subsequent stages.
mlir::LogicalResult mapPrintBridgeRocketFASEDReadLatency(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Consume write deadlines through the ten-entry flow-through latency queue.
// Atomic; preserve expanded Print banks and read completion boundaries.
// Split AW/W completion pairing and latency register attachment follow.
mlir::LogicalResult mapPrintBridgeRocketFASEDWriteLatency(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Bind accepted AW IDs through the ten-entry timing queue to write completion.
// Atomic; retain expanded Print banks. AW/W pairing remains an explicit pulse.
mlir::LogicalResult mapPrintBridgeRocketFASEDTimingAWQueue(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Pair accepted AW and final W handshakes using target-qualified saturating
// counters. Atomic; retain expanded Print banks. B retirement and maximum stay
// explicit inputs until timing admission and latency MMIO attachment.
mlir::LogicalResult mapPrintBridgeRocketFASEDWritePairing(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Close target B retirement feedback through the response-releaser hierarchy.
// Atomic; append internal observation ports while retaining existing modules,
// expanded Print banks and allocation. Runtime maximum MMIO attachment follows.
mlir::LogicalResult mapPrintBridgeRocketFASEDWriteRetirement(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Close AW/W reverse readiness with the pending-full observations. Atomic;
// preserve expanded banks and AR readiness. Runtime maximum remains external.
mlir::LogicalResult mapPrintBridgeRocketFASEDWriteAdmission(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Close AR pending-full admission and accepted final-R retirement. Atomic;
// retain existing modules and expanded banks. Runtime maximum remains external.
mlir::LogicalResult mapPrintBridgeRocketFASEDReadAdmission(
    circt::firrtl::CircuitOp circuit, std::string &error);

/// Attach the allocated request-limit bank after read/write admission.
mlir::LogicalResult mapPrintBridgeRocketFASEDRequestLimits(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach the allocated latency bank after request limits. Requires the Rocket
/// allocation and raw annotation archive; consumes the two latency inputs, adds
/// one MCR output and wrapper, retargets copied ports, and preserves prior modules.
mlir::LogicalResult mapPrintBridgeRocketFASEDLatencyRegisters(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach the allocated functional-model register after latency registers.
/// Atomic; consume ingress relaxation, retarget copied ports and the archive,
/// and preserve prior modules and the expanded Rocket/Print allocation.
mlir::LogicalResult mapPrintBridgeRocketFASEDFunctionalModelRegister(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach the allocated read-only response-error bank after the functional-model
/// register. Atomic; observe accepted host responses, retarget copied ports and
/// the archive, and preserve prior modules and expanded Rocket/Print allocation.
mlir::LogicalResult mapPrintBridgeRocketFASEDResponseErrors(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach allocated statistics after response errors, exposing accepted target R
/// beats through the unique response hierarchy. Atomic; retain module identities
/// and all original port arguments, retarget the archive, export four MCR words.
mlir::LogicalResult mapPrintBridgeRocketFASEDStatistics(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach allocated occupancy histograms after statistics. Atomic; observe
/// pre-edge pending reads/AW, preserve prior module/port/argument identities,
/// transfer the archive and expose ten read-only MCR words, including zero bins.
mlir::LogicalResult mapPrintBridgeRocketFASEDHistograms(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Assemble the six exported fragments after histograms into the 21-word FASED
/// bank. Atomic; preserve expanded allocation and prior module/port identities,
/// transfer copied/indexed targets and retain whole-fragment references inside.
mlir::LogicalResult mapPrintBridgeRocketFASEDMMIOBank(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach and bind the native FASED MCRFile using the live expanded allocation.
/// Stage both wrappers atomically; preserve existing banks and Print interfaces.
mlir::LogicalResult mapPrintBridgeRocketFASEDControl(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Close the completed expanded Rocket/Print control fabric with the native
/// five-channel host master. Atomic; validate live allocation and slave closure,
/// preserve prior IR identities and transfer copied annotation targets.
mlir::LogicalResult mapPrintBridgeRocketControlMaster(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Assemble the expanded Rocket/Print FASED split host requests/responses into
/// the native pre-translation AXI memory master. Atomic; preserve prior IR
/// identities and transfer mapped leaf and copied annotation targets.
mlir::LogicalResult mapPrintBridgeRocketFASEDHostMemory(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Translate the expanded Rocket/Print memory master into the recorded host
/// region. Atomic; preserve prior IR identities and pre-translation memory
/// targets, transfer copied targets and retain valid-qualified bounds checks.
mlir::LogicalResult mapPrintBridgeRocketFASEDAddressTranslation(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Deinterleave expanded Rocket/Print read responses before translation with
/// sixteen eight-beat queues. Atomic; preserve existing IR identities/targets.
mlir::LogicalResult mapPrintBridgeRocketFASEDReadDeinterleaver(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Buffer all five translated AXI channels with independent depth-two queues.
/// Atomic; preserve the expanded Print/Rocket module, port and target identities.
mlir::LogicalResult mapPrintBridgeRocketFASEDHostMemoryBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach the catalogued LoadMem writer, queues and nine decoded MCR words.
/// Atomic; retain existing modules and transfer copied top port targets.
mlir::LogicalResult mapPrintBridgeRocketLoadMemEngine(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Arbitrate expanded LoadMem/FASED AW and reserve W source until accepted WLAST.
/// Atomic; retain prior IR and consumed targets, transfer copied public targets.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryWriteArbiter(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Route host B by LoadMem ID 16 / FASED IDs 0..15; join the host AW/W/B port.
/// Atomic; retain consumed B targets and transfer copied ports and AR/R/AW/W.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryWriteResponses(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Arbitrate host AR, retain stalled selection and widen FASED IDs to five bits.
/// Atomic; retain consumed AR targets, transfer R and join AW/W/B/AR targets.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryReadArbiter(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Route host R by LoadMem ID 16 / FASED IDs 0..15 and join all AXI channels.
/// Atomic; preserve consumed R targets and transfer copied ports and AW/W/B/AR.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryReadResponses(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Buffer the unified host master with five independent no-flow/no-pipe queues.
/// Atomic; retain prior modules, top ports, SSA values and archived targets.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryOutputBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Expose the buffered host master as the platform's 34/64/16 mem_0 port.
/// Atomic; preserve prior IR and consumed targets, transfer copied ports.
mlir::LogicalResult mapPrintBridgeRocketHostMemoryPort(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach zero-sink CPU AW/W/B defaults and reset-gated protocol assertions.
/// Atomic; preserve prior IR and transfer copied top targets.
mlir::LogicalResult mapPrintBridgeRocketEmptyCPUStreamWrite(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach independent depth-two AW/W queues to the expanded CPU write boundary.
/// Atomic; retain existing identities and consumed AW/W targets.
mlir::LogicalResult mapPrintBridgeRocketCPUStreamWriteBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Materialize zero-sink AW/W/B and independent depth-two AW/W queues for the
/// selected Print control master. Validate live allocated collateral first;
/// atomically preserve existing IR identities and copied/consumed targets.
mlir::LogicalResult mapPrintBridgeCPUStreamWriteBuffer(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Attach an independent depth-two AR queue to the selected Print AW/W boundary.
/// Validate live allocation; atomically retain prior IR and consumed AR targets.
mlir::LogicalResult mapPrintBridgeCPUStreamReadBuffer(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Attach a depth-two R response queue to the selected Print AR boundary.
/// Validate live allocation; atomically retain prior IR and consumed R targets.
mlir::LogicalResult mapPrintBridgeCPUStreamResponseBuffer(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Attach a depth-two B response queue to the selected Print R boundary.
/// Validate live allocation; preserve zero-sink RAM reads and prior IR identities.
mlir::LogicalResult mapPrintBridgeCPUStreamWriteResponseBuffer(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Expose selected buffered CPU streams as the U250 64/512/16 AXI slave.
/// Validate live allocation; preserve all prior SSA and CPU scalar identities.
mlir::LogicalResult mapPrintBridgeCPUStreamPort(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    std::string &error);
/// Attach an independent depth-two AR queue to the expanded CPU AW/W boundary.
/// Atomic; retain existing identities and consumed AR targets.
mlir::LogicalResult mapPrintBridgeRocketCPUStreamReadBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach a depth-two R queue to the expanded CPU AR boundary.
/// Atomic; retain existing identities and consumed R targets.
mlir::LogicalResult mapPrintBridgeRocketCPUStreamResponseBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Attach a depth-two B queue to the expanded CPU R boundary.
/// Atomic; retain existing identities and consumed B targets.
mlir::LogicalResult mapPrintBridgeRocketCPUStreamWriteResponseBuffer(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Expose buffered CPU streams as the platform's 64/512/16 AXI slave.
/// Atomic; preserve existing IR and CPU scalar targets, transfer copied ports.
mlir::LogicalResult mapPrintBridgeRocketCPUStreamPort(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Assemble the five-port U250 shell around the expanded Print CPU platform.
/// Requires FAME host identities; preserve all old IR and private targets.
mlir::LogicalResult mapPrintBridgeRocketFPGATopShell(
    circt::firrtl::CircuitOp circuit, std::string &error);
/// Specialize the expanded Print shell for the one-channel U250 platform.
/// Preserve old IR; add host request ID counters and DDR/XDC annotations.
mlir::LogicalResult mapPrintBridgeRocketF1Shim(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
