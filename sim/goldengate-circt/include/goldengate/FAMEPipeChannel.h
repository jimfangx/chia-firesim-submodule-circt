// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Materialize a two-entry scalar PipeChannel. Latency one inserts a zero
// token after reset; latency zero starts empty, as in Scala PipeChannel.
mlir::LogicalResult addFAMEPipeChannel(circt::firrtl::CircuitOp circuit,
                                      unsigned payloadWidth, unsigned latency,
                                      std::string &error);
// Discover scalar boundary pipes from retained channel annotations and create
// one module definition for each (payload width, latency) pair.
mlir::LogicalResult addFAMEBoundaryPipeChannels(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Connect each annotated boundary pipe in a simulator-facing wrapper, using
// the endpoint's port direction to distinguish model and bridge sources.
// ReadyValid channels initially pass through and are replaced by the following
// ReadyValidChannel transform. Clock channels still pass through.
mlir::LogicalResult addFAMEPipeWrapper(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
// Select the wrapper as the FIRRTL circuit top and move retained annotation
// targets from its formerly direct FAMETop ports to the wrapper ports.
mlir::LogicalResult activateFAMEPipeWrapper(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
