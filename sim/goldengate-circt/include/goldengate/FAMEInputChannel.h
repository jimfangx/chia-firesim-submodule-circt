// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEPortAnalysis.h"
#include <string>

namespace goldengate {
// Replace the model inputs and top-level connections belonging to one data
// channel with the corresponding typed decoupled bundle.
// Wrapper ground-port DontTouch annotations and wrapper/model inner symbols
// move to payload field IDs; symbol names/visibility and InnerRefs stay unchanged.
// Model annotations must already be consumed; identities add no protection.
// Unsupported attached metadata is rejected before the channel is changed.
// For additional instances of an already rewritten scalar definition, set
// rewriteModel=false. The definition must have the exact channel ABI at the
// same index; only the instance and its wrapper ports are then changed.
// Callers must refresh port analyses and inner symbol tables after rewriting.
mlir::LogicalResult rewriteFAMEInputChannel(const TopHierarchy &hierarchy,
                                             const FAMETopChannelPort &channel,
                                             std::string &error,
                                              bool rewriteModel = true);

// Channelize all ordered hub Clock inputs and explicitly transfer retained
// ChannelConnection sinks/ChannelPorts ports to scalar bits or bundle leaves.
// Transfer associated clock/clockPort references to erased input clocks to the
// same Clock leaves; preserve surviving output aliases and shared references.
// Optionally transfer both spellings of private FPGA debug targets. Preflight
// domain identities, payload/retained target order and unique occurrences before
// mutation; preserve unrelated annotations, list order and attached identities
// under rewriteFAMEInputChannel's policy. Refresh hierarchy/port analyses.
mlir::LogicalResult rewriteFAMEHubClockChannel(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    llvm::ArrayRef<FAMEHubClockDomain> domains, bool transferDebug,
    std::string &error);

// Introduce the clock token alongside the original target clock so the gate
// can replace its uses before the scalar port is removed.
mlir::LogicalResult addFAMEClockChannelToken(
    circt::firrtl::FModuleOp top, circt::firrtl::FModuleOp model,
    llvm::StringRef instanceName, llvm::StringRef topClockName,
    llvm::StringRef modelClockName, llvm::StringRef topChannelName,
    llvm::StringRef modelChannelName, circt::firrtl::BundleType channelType,
    std::string &error);

// Latch the next target-clock token at the end of a simulated target cycle.
// The register is subsequently used by the abstract target clock gate. An
// absent token selects SFC's VirtualClockChannel (constant one).
// Requires hostClock: Clock, hostReset/targetCycleFinishing: UInt<1>, and a
// nonempty model clock identity without an existing generated enable.
// Consumes/produces no annotations; creates a host-clocked, reset-to-zero
// register and its completion mux. Uniques its name through all model ports
// and declarations, preserving target state; attaches fameClockEnable identity.
// Channel/hierarchy analyses are unchanged; callers must refresh state scans.
mlir::LogicalResult addFAMEClockEnable(circt::firrtl::FModuleOp model,
                                       llvm::StringRef modelClockName,
                                       mlir::Value clockTokenBits,
                                       std::string &error);

// Instantiate the SFC abstract clock gate and replace model uses of the target
// clock with its gated host clock output. When the clock is already a channel,
// preserve the raw token bits used by the clock-enable register.
// Accept a passive Clock leaf within the model input's bits payload. Match
// replacement reads by CIRCT root/field identity across separate selector
// trees; sibling clock leaves keep their independent tokens and target uses.
// Reuse the circuit's compatible AbstractClockGate external definition,
// preserving its annotations and symbols. Reject incompatible definitions
// before mutation; every model/domain gets its own instance and CE operands.
// Resolve buffered enables by fameClockEnable identity (legacy names only in
// untagged transformed boundaries); require model host clock/reset and reset0.
// Allocate the instance through the complete namespace and preserve its
// original clock with fameClockGate identity for later XDC attachment.
mlir::LogicalResult addFAMEClockGate(circt::firrtl::CircuitOp circuit,
                                     circt::firrtl::FModuleOp model,
                                     llvm::StringRef modelClockName,
                                     std::string &error,
                                     mlir::Value rawClockTokenBits = {});

// Remove the original target clock from the top, model, and model instance
// after all model uses have been replaced with the gated host clock. Require
// unannotated, unsymbolized scalar ports; clock-input identity transfer is not
// defined by SFC's hostDecouplingRenames. Reject instance-port annotations before
// mutation and preserve unrelated instance metadata when rebuilding its ports.
mlir::LogicalResult removeFAMETargetClockPort(
    circt::firrtl::FModuleOp top, circt::firrtl::FModuleOp model,
    llvm::StringRef instanceName, llvm::StringRef topClockName,
    llvm::StringRef modelClockName, std::string &error);

// Remove a consumed non-hub target clock and its ancillary writes at every
// instance of the model. The scalar model input must be unused, unannotated,
// and have no inner symbol. Instance reads/annotations are rejected before
// mutation. Parent clock sources are preserved for other users. Invalidates
// hierarchy/port analyses; consumes no annotations and produces none.
mlir::LogicalResult removeFAMEVirtualClockPort(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp model,
    llvm::StringRef modelClockName, std::string &error);

// Clock outputs used to identify channel domains are no longer simulator
// ports after FAME. Keep their model-side connects by turning each into a wire.
// Require one model instance and exclusively connected, unannotated clock
// outputs. Resolve wrapper aliases by their direct strict/ordinary SSA connect;
// promoted wrapper port names need not match model port names. Transfer model
// ground-port inner symbols to same-name Clock wires,
// preserving names/visibility and InnerRefs. Wrapper symbols and instance-port
// annotations need a deletion policy and are rejected before mutation. Preserve
// unrelated ports and instance metadata; refresh hierarchy and symbol analyses.
// Consumes/produces no annotations.
mlir::LogicalResult internalizeFAMEOutputClocks(
    circt::firrtl::FModuleOp top, circt::firrtl::FModuleOp model,
    llvm::StringRef instanceName, std::string &error);

// Outputs absent from the model channel graph (e.g. promoted passthroughs)
// become same-name internal wires, preserving their model-side assignments.
// Require passive, unannotated data outputs with unused results at every
// instance. Transfer scalar and aggregate-leaf inner symbols unchanged to the
// same-type wires. Aggregate-root/sub-bundle symbols require a LowerTypes
// policy and are rejected. Preflight all identities and instances before mutation;
// retain parent wiring and metadata. Refresh hierarchy, port and inner-symbol
// analyses after rewriting; emits no annotations.
mlir::LogicalResult internalizeFAMEUnusedOutputs(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp model,
    llvm::ArrayRef<llvm::StringRef> portNames, std::string &error);

// Before adding FAME wiring, remove original scalar Clock-destination connects
// throughout the wrapper. Temporarily retain direct wrapper/model clock port
// connects for the later target/output clock port helpers to consume. Preserve
// declarations, data connects and model bodies; consumes no annotations.
mlir::LogicalResult removeFAMEAncillaryTopClockConnects(
    circt::firrtl::FModuleOp top, circt::firrtl::InstanceOp modelInstance,
    std::string &error);

// After model clock/channel rewriting, drop remaining scalar wrapper clocks
// other than hostClock and their direct ancillary clock connects. Require an
// uninstantiated top and unannotated, unsymbolized ports used only by clock
// connects. Preflight all uses before mutation; retain host/channel clocks,
// data wiring and declarations. Invalidates top port/hierarchy analyses.
mlir::LogicalResult removeFAMEStaleTopClocks(
    circt::firrtl::FModuleOp top, std::string &error);

// Place host controls and the model clock sink before data sinks, followed by
// data sources, as in the SFC FAME model interface. Keep each group's order.
// An empty modelClockSink selects the non-hub virtual-clock interface.
// Reorder the shared definition and every direct instance in top together.
mlir::LogicalResult groupFAMEChannelPorts(
    circt::firrtl::FModuleOp top, circt::firrtl::FModuleOp model,
    llvm::StringRef instanceName, llvm::StringRef modelClockSink,
    std::string &error);

// Retain non-stale wrapper ports in their original pre-FAME order, then append
// channels in ChannelConnectionAnnotation order, with sinks before sources.
// Both lists form the complete final interface; reject missing/duplicate ports
// before mutation. Reorder metadata and SSA block arguments together.
mlir::LogicalResult orderFAMETopPorts(
    circt::firrtl::FModuleOp top,
    llvm::ArrayRef<llvm::StringRef> retainedPortNames,
    llvm::ArrayRef<llvm::StringRef> channelPortNames, std::string &error);
} // namespace goldengate
