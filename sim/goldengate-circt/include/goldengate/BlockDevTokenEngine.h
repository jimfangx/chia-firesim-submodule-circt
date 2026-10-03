// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_TOKEN_ENGINE_H
#define GOLDENGATE_BLOCKDEV_TOKEN_ENGINE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// One-tracker BlockDev HostPort gates, target cycle counter and write-beat
// tracking. Completion enqueue valid remains an explicit latency-pipe boundary.
// Functional queues, MMIO settings and response scheduling are mapped later.
mlir::LogicalResult addBlockDevTokenEngine(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
