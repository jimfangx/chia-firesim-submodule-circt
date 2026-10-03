// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITETRACKER_H
#define GOLDENGATE_CONTROLWRITETRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult addControlWriteTracker(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
