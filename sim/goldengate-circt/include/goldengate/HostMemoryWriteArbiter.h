// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
/// Compose the recorded host AW/W arbiter at the buffered translation top or
/// its Print LoadMem attachment, validating direct source and clock forwarding.
mlir::LogicalResult addHostMemoryWriteArbiter(circt::firrtl::CircuitOp circuit,
                                             std::string &error);
}
