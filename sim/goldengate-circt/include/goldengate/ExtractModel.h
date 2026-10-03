// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
mlir::LogicalResult extractModels(circt::firrtl::CircuitOp circuit,
                                  unsigned &promoted, std::string &error);
}
