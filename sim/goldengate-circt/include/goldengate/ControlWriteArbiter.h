// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITEARBITER_H
#define GOLDENGATE_CONTROLWRITEARBITER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// U250 NastiRouter's twelve-source RRArbiter and accepted B response signals.
mlir::LogicalResult addControlWriteArbiter(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
