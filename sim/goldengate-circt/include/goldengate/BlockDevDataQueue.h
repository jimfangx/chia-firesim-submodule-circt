// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_DATA_QUEUE_H
#define GOLDENGATE_BLOCKDEV_DATA_QUEUE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind target BlockDev data to a 32-entry FIFO of tag/64-bit data beats.
// Host dequeue remains an explicit boundary for the MMIO bank.
mlir::LogicalResult addBlockDevDataQueue(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
#endif
