// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLTRANSACTIONTRACKER_H
#define GOLDENGATE_CONTROLTRANSACTIONTRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
namespace goldengate {
// NastiRouter ReorderQueue with twelve-bit transaction tags: 64 low-tag slots,
// six upper tag bits, ceil(log2(slaveCount + 1)) route bits and slaveCount + 1
// retirement ports (including the error slave). slaveCount must be 1..63.
// Tag and route state have no reset; only free bits reset. Invalid counts return
// an empty port list/null module without mutating the circuit.
llvm::SmallVector<circt::firrtl::PortInfo> controlTransactionTrackerPorts(mlir::MLIRContext *context,
    unsigned slaveCount = 11);
// queueName supplies NastiRouter's assertion identity (ar_queue or aw_queue).
circt::firrtl::FModuleOp createControlTransactionTracker(circt::firrtl::CircuitOp circuit,
                                                        llvm::StringRef name,
                                                        llvm::StringRef queueName,
                                                        unsigned slaveCount = 11);
}
#endif
