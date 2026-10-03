// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_WRITE_LATENCY_H
#define GOLDENGATE_BLOCKDEV_WRITE_LATENCY_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Connect the write tracker completion to a one-entry DynamicLatencyPipe.
// Response scheduling consumes the exposed dequeue boundary in a later pass.
mlir::LogicalResult addBlockDevWriteLatency(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
