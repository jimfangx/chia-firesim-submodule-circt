// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADARBITER_H
#define GOLDENGATE_CONTROLREADARBITER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// U250 NastiRouter's twelve-source HellaPeekingArbiter. Accepted last beats
// release the burst lock and retire the existing AR tracker slots.
mlir::LogicalResult addControlReadArbiter(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
