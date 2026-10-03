// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_MMIO_BANK_H
#define GOLDENGATE_BLOCKDEV_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind the one-tracker BlockDev queues to the 26-word decoded register bank.
mlir::LogicalResult addBlockDevMMIOBank(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
}
#endif
