// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult addFASEDIngressCredits(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
