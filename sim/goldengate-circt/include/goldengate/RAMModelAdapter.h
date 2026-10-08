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
// Unsupported readwrite ports, cross-command channel sharing, widths, target paths or implementation ABIs
// fail without changing the wrapper. Depth for the SFC async implementation is
// 2^addressWidth (which can differ from the original FIRRTL memory depth).
mlir::LogicalResult wrapRAMModel(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp wrapper,
    circt::firrtl::FModuleOp implementation, RAMModelParameters &parameters,
    std::string &error);
}
