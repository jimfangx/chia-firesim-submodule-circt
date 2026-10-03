// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_READ_LATENCY_H
#define GOLDENGATE_BLOCKDEV_READ_LATENCY_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Connect the committed read request to a one-entry DynamicLatencyPipe with unreset payload memory.
// Response scheduling consumes the exposed dequeue boundary in a later pass.
mlir::LogicalResult addBlockDevReadLatency(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
