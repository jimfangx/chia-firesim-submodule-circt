// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_DRAM_BACKEND_H
#define GOLDENGATE_FASED_DRAM_BACKEND_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected DDR3 backend: independent four-entry read/write DynamicLatencyPipes.
// Ports: clock, reset, targetFire, newReadValid, newReadReady, newReadID:4,
// newReadLen:8, newWriteValid, newWriteReady, newWriteID:4,
// completedReadValid, completedReadReady, completedReadID:4,
// completedReadLen:8, completedWriteValid, completedWriteReady,
// completedWriteID:4, readLatency:12, writeLatency:12, tCycle:12,
// readCount:3, writeCount:3. Ready/valid/count are combinational; targetFire
// gates all state and synchronous reset. Payload/deadlines are unreset.
// Deadlines expire on modulo equality only; pending clears even off-head.
// Enqueue overrides expiry. No flow-through or full enqueue/dequeue bypass.
// A free helper symbol is required; rejection leaves circuit/result unchanged.
mlir::LogicalResult materializeFASEDDRAMBackend(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
