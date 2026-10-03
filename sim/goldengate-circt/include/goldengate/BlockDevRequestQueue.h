// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_REQUEST_QUEUE_H
#define GOLDENGATE_BLOCKDEV_REQUEST_QUEUE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind the BlockDev token engine to a 10-entry request queue. Host dequeue
// remains an explicit boundary for the scheduler and MMIO bank.
mlir::LogicalResult addBlockDevRequestQueue(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
