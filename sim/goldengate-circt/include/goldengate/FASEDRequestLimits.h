// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_REQUEST_LIMITS_H
#define GOLDENGATE_FASED_REQUEST_LIMITS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Decode lanes 0/1 are the write/read maximum words at byte offsets 8/12.
mlir::LogicalResult addFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
