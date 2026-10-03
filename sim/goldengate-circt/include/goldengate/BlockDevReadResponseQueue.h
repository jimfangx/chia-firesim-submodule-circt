// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_READ_RESPONSE_QUEUE_H
#define GOLDENGATE_BLOCKDEV_READ_RESPONSE_QUEUE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Buffer host read responses in the existing 32-entry, 65-bit FIFO module.
mlir::LogicalResult addBlockDevReadResponseQueue(circt::firrtl::CircuitOp circuit,
                                               std::string &error);
}
#endif
