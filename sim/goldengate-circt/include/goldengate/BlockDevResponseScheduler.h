// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_RESPONSE_SCHEDULER_H
#define GOLDENGATE_BLOCKDEV_RESPONSE_SCHEDULER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind write-priority response scheduling to the token engine and latency pipes.
mlir::LogicalResult addBlockDevResponseScheduler(circt::firrtl::CircuitOp circuit,
                                                std::string &error);
}
#endif
