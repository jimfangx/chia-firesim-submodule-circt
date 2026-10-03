// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Attach the local channel port groups inferred from the excised top-level
// channel connections to the retained FAME annotation stream.
mlir::LogicalResult inferModelPorts(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
} // namespace goldengate
