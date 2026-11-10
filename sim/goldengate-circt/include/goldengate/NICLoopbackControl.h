// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Prove that SFC's three unspecified pauser payloads cannot reach live logic.
// Accepts a mixed production bridge catalog; changes no IR or annotations.
mlir::LogicalResult validateNICLoopbackPauserInputs(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Production token/allocated-disabled-CPU queue stage. Preserves target,
// memory, clock and other bridge owners for the ordinary platform pipeline.
// Requires the same post-FAME stream boundary as normal addNICTokenQueues.
// No NIC is a no-op; failures leave the circuit unchanged.
mlir::LogicalResult addNICLoopbackPlatformQueues(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Compose an explicitly selected, constructor-free NIC-only post-FAME stream
// boundary into shared global control and the fully buffered F2 CPU AXI port.
// Retain two allocated but disabled CPU queues and the one-word done bank per
// widget. Emit the native free-running loopback driver plus CPU-engine header
// and peer map from the live allocations. The three SFC-undriven pauser payloads
// must be provably unused through ordinary wiring/internal hierarchy; a live
// pauser (including usePauser=true) rejects rather than inventing semantics.
// This does not synthesize clocks, target models, memory bridges or an F2 shell.
// Clone/verify/commit: IR and output strings remain unchanged on any failure.
mlir::LogicalResult compileNICLoopbackControl(
    circt::firrtl::CircuitOp circuit, unsigned addressBits,
    std::string &header, std::string &peerMap, std::string &error);
}
