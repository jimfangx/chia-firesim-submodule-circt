// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
struct RAMModelParameters {
  unsigned addressWidth = 0, dataWidth = 0, reads = 0, writes = 0;
};
// EmitAndWrapRAMModels' channel adapter, after FAME1. The implementation is an
// already elaborated host RAM module with the aggregate RegfileModelIO ABI.
// This boundary does not generate or infer its timing model. Resolve retained
// MemPortAnnotation targets to typed CIRCT ports, validate the entire ABI, and
// replace only the selected wrapper body. Keep ports and annotations unchanged.
// Read/write order follows distinct retained annotations; consumers must bind
// commands by their address payload rather than generated vector indices.
// Input channels shared across commands use independent delivery tracking:
// hold each token/payload until all consumers accept it, suppress repeat
// deliveries, and flush protocol state on host reset without clearing memory.
// Distinct response fields sharing an output channel join their valid/ready
// handshakes, so an incomplete result packet never escapes under skew.
// Unsupported readwrite ports, multiply-driven response fields, widths, target paths or implementation ABIs
// fail without changing the wrapper. Depth for the SFC async implementation is
// 2^addressWidth (which can differ from the original FIRRTL memory depth).
mlir::LogicalResult wrapRAMModel(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp wrapper,
    circt::firrtl::FModuleOp implementation, RAMModelParameters &parameters,
    std::string &error);
// Complete selected-wrapper EmitAndWrapRAMModels boundary. Resolve the channel
// ABI, allocate a unique RamModel, emit its native async timing operations, and
// replace the wrapper body. No elaborated host declaration/body is required.
// Returns the created implementation; all failures leave the circuit unchanged.
mlir::LogicalResult materializeRAMModel(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp wrapper,
    circt::firrtl::FModuleOp &implementation, RAMModelParameters &parameters,
    std::string &error);
// Materialize every selected post-FAME RAM wrapper discovered through the
// typed addr targets of retained ModelRead/Write/ReadWritePort annotations.
// Repeated port records select each definition once. Geometry and command
// layout come from the wrapper's channel types via materializeRAMModel.
// Verify all replacements on a private circuit clone before replacing any
// original body or adding a host, so a late unsupported wrapper leaves the
// entire circuit and materialized count unchanged. Successful replacement
// preserves original module operations, ports, annotations and untouched SSA;
// selected wrapper bodies change. No selections succeeds without IR changes.
mlir::LogicalResult materializeSelectedRAMModels(
    circt::firrtl::CircuitOp circuit, unsigned &materialized,
    std::string &error);
}
