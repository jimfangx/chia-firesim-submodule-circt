// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
mlir::LogicalResult wrapTop(circt::firrtl::CircuitOp circuit,
                            std::string &error);
} // namespace goldengate
