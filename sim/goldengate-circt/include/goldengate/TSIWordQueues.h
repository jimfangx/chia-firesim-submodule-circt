// See LICENSE for license details.
#ifndef GOLDENGATE_TSI_WORD_QUEUES_H
#define GOLDENGATE_TSI_WORD_QUEUES_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind the TSI token engine to two 16-entry queues of 32-bit words.
// Host enqueue/dequeue remain explicit boundaries for the MMIO bank.
mlir::LogicalResult addTSIWordQueues(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
}
#endif
