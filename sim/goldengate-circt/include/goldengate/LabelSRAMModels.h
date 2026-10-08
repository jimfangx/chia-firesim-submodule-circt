// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Optional LabelSRAMModels boundary. Requires native firrtl.mem operations.
// All selected memories are checked before changing IR or retained metadata.
mlir::LogicalResult labelSRAMModels(circt::firrtl::CircuitOp circuit,
                                  unsigned &extracted, std::string &error);
} // namespace goldengate
