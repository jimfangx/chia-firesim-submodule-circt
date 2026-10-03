// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWIDGETWRITES_H
#define GOLDENGATE_CONTROLWIDGETWRITES_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind the seven implemented U250 MCRFile slaves to NastiRouter's AW/W
// dispatcher. The remaining control bundles contain only AR/B/R. Additional
// AW/W metadata is broadcast from shared master ports, as in NastiRouter.
// Unimplemented slaves and response arbitration remain explicit boundaries.
mlir::LogicalResult bindControlWidgetWrites(circt::firrtl::CircuitOp circuit,
                                           std::string &error);
}
#endif
