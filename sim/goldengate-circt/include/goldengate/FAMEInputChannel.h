// See LICENSE for license details.
#pragma once

#include "goldengate/FAMEPortAnalysis.h"
#include <string>

namespace goldengate {
// Replace the model inputs and top-level connections belonging to one data
// channel with the corresponding typed decoupled bundle.
mlir::LogicalResult rewriteFAMEInputChannel(const TopHierarchy &hierarchy,
                                             const FAMETopChannelPort &channel,
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
// unique <modelClockName>_enabled name. Consumes/produces no annotations;
// creates a host-clocked, reset-to-zero register and its completion mux.
// Channel/hierarchy analyses are unchanged; callers must refresh state scans.
mlir::LogicalResult addFAMEClockEnable(circt::firrtl::FModuleOp model,
                                       llvm::StringRef modelClockName,
                                       mlir::Value clockTokenBits,
                                       std::string &error);

// Instantiate the SFC abstract clock gate and replace model uses of the target
// clock with its gated host clock output. When the clock is already a channel,
// preserve the raw token bits used by the clock-enable register.
// Reuse the circuit's compatible AbstractClockGate external definition,
// preserving its annotations and symbols. Reject incompatible definitions
// before mutation; every model/domain gets its own instance and CE operands.
mlir::LogicalResult addFAMEClockGate(circt::firrtl::CircuitOp circuit,
                                     circt::firrtl::FModuleOp model,
                                     llvm::StringRef modelClockName,
                                     std::string &error,
                                     mlir::Value rawClockTokenBits = {});

// Remove the original target clock from the top, model, and model instance
// after all model uses have been replaced with the gated host clock.
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
mlir::LogicalResult internalizeFAMEOutputClocks(
    circt::firrtl::FModuleOp top, circt::firrtl::FModuleOp model,
    llvm::StringRef instanceName, std::string &error);

// Outputs absent from the model channel graph (e.g. promoted passthroughs)
// become same-name internal wires, preserving their model-side assignments.
// Require passive, unannotated, unsymbolized data outputs with unused results
// at every instance. Preflight all instances before mutation; retain parent
// wiring and metadata. Invalidates hierarchy/port analyses; emits no annotations.
mlir::LogicalResult internalizeFAMEUnusedOutputs(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp model,
    llvm::ArrayRef<llvm::StringRef> portNames, std::string &error);

// Place host controls and the model clock sink before data sinks, followed by
// data sources, as in the SFC FAME model interface. Keep each group's order.
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
