// See LICENSE for license details.
#ifndef GOLDENGATE_TRACERVHEADER_H
#define GOLDENGATE_TRACERVHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Atomically append every TracerV constructor, checked against its live trigger
// registers, queue, shared CPU stream allocation and rational clock identity.
mlir::LogicalResult prepareTracerVHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
