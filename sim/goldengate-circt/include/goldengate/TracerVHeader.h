// See LICENSE for license details.
#ifndef GOLDENGATE_TRACERVHEADER_H
#define GOLDENGATE_TRACERVHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append the TracerV ABI checked against live trigger registers, stream and clock.
mlir::LogicalResult prepareTracerVHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
