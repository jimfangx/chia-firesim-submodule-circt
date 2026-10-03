// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLTRANSACTIONTRACKER_H
#define GOLDENGATE_CONTROLTRANSACTIONTRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
namespace goldengate {
// U250 ReorderQueue: 64 low-tag slots, six upper tag bits, four route bits,
// twelve retirements. Tag and route state have no reset; only free bits reset.
llvm::SmallVector<circt::firrtl::PortInfo> controlTransactionTrackerPorts(mlir::MLIRContext *context);
// queueName supplies NastiRouter's assertion identity (ar_queue or aw_queue).
circt::firrtl::FModuleOp createControlTransactionTracker(circt::firrtl::CircuitOp circuit,
                                                        llvm::StringRef name,
                                                        llvm::StringRef queueName);
}
#endif
