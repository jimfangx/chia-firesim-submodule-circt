// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_WRITE_ACK_QUEUE_H
#define GOLDENGATE_BLOCKDEV_WRITE_ACK_QUEUE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Buffer host write acknowledgements in a four-entry scalar-tag FIFO.
mlir::LogicalResult addBlockDevWriteAckQueue(circt::firrtl::CircuitOp circuit,
                                           std::string &error);
}
#endif
