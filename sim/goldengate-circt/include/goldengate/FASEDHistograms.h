// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_HISTOGRAMS_H
#define GOLDENGATE_FASED_HISTOGRAMS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Recorded occupancy bins 0,2,4,8; decoded read-only MCR offsets 16-52.
mlir::LogicalResult addFASEDHistograms(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
