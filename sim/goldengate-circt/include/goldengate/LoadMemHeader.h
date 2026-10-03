// See LICENSE for license details.
#ifndef GOLDENGATE_LOADMEMHEADER_H
#define GOLDENGATE_LOADMEMHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Emit LoadMem's core driver constructor from live control and FIFO geometry.
mlir::LogicalResult prepareLoadMemHeader(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
#endif
