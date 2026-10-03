// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_STATISTICS_H
#define GOLDENGATE_FASED_STATISTICS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Decoded lanes 0-3: W/R beats and AW/AR transactions, byte offsets 56-68.
mlir::LogicalResult addFASEDStatistics(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
