// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADTRACKER_H
#define GOLDENGATE_CONTROLREADTRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// U250 NastiRouter's 64-slot, 12-bit-ID ReorderQueue. Enqueue capacity
// feeds the existing AR dispatch; accepted last R beats retire each slot.
// Four unmapped slave completion interfaces remain explicit boundaries.
mlir::LogicalResult addControlReadTracker(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
