// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADDISPATCH_H
#define GOLDENGATE_CONTROLREADDISPATCH_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter AR DecoupledHelper gates, readiness priority and payload broadcast.
// Bind the seven mapped MCRFile read interfaces and the existing error slave.
// Response tracker enqueue capacity remains an explicit input until its
// ReorderQueue is ported; emit enqueue valid/tag/target for that next stage.
mlir::LogicalResult addControlReadDispatch(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
