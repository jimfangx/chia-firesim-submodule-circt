// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Materialize a two-entry UInt PipeChannel. Latency one inserts a zero
// token after reset; latency zero starts empty, as in Scala PipeChannel.
mlir::LogicalResult addFAMEPipeChannel(circt::firrtl::CircuitOp circuit,
                                      unsigned payloadWidth, unsigned latency,
                                      std::string &error);
// Typed payloads may be UInt/SInt leaves, passive bundles, or vectors of those
// types with known widths (including zero). A complete payload is one token;
// all fields share the same queue occupancy, stall, and initialization rules.
// Reject unsupported types before creating any module or operation.
// Requires: a live circuit, non-const payload type, latency zero or one, and
// no colliding module symbol. Consumes/produces no annotations. Mutates: adds
// a FIRRTL module containing host-clocked queue state and handshake logic.
// Requires no analyses; preserves existing module/port identities. Rebuild
// cached instance/symbol analyses after construction. Produces a verified
// passive payload interface with two token slots and typed zero initialization.
mlir::LogicalResult addFAMEPipeChannel(circt::firrtl::CircuitOp circuit,
                                      circt::firrtl::FIRRTLBaseType payloadType,
                                      unsigned latency, std::string &error);
// Discover single-endpoint boundary pipes from retained channel annotations
// and create one module definition for each (payload type, latency) pair.
// Requires: post-FAME Decoupled ports and retained PipeChannel annotations
// naming complete bits fields with matching direction. Consumes/produces no
// annotations. Validates every endpoint and symbol before adding definitions;
// mutation/analysis/output contracts are otherwise the same as above.
mlir::LogicalResult addFAMEBoundaryPipeChannels(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Connect each annotated boundary pipe in a simulator-facing wrapper, using
// the endpoint's port direction to distinguish model and bridge sources.
// Validate every queue definition's port names, directions and exact payload
// types before creating the wrapper; incompatible definitions fail atomically.
// ChannelFanout groups of bridge-sourced sinks share one external primary
// input and broadcast atomically to independent queues. Secondary inputs are
// omitted; their annotation targets retain the inner target module identity.
// Validate group membership and equal payload types before any mutation.
// ReadyValid channels initially pass through and are replaced by the following
// ReadyValidChannel transform. Clock channels still pass through.
mlir::LogicalResult addFAMEPipeWrapper(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
// Select the wrapper as the FIRRTL circuit top and move retained annotation
// targets from its formerly direct FAMETop ports to the wrapper ports.
// ChannelConnection.clock retains its inner module identity (only its circuit
// name changes): a target Clock leaf is not a wrapper Boolean clock token.
// Requires an inactive GGFAMEPipeWrapper and retained rawAnnotations. Consumes
// no annotations; produces retargeted annotations and selects the circuit top.
// Mutates circuit identity and annotation targets, preserving module IR and
// target-domain identities. No analysis is required; cached circuit/target
// analyses must be rebuilt after activation. Boundary endpoints name wrapper
// ports while associated clocks continue to name inner target ports.
mlir::LogicalResult activateFAMEPipeWrapper(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
