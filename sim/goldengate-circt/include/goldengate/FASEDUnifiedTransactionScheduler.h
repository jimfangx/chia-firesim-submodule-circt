// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_UNIFIED_TRANSACTION_SCHEDULER_H
#define GOLDENGATE_FASED_UNIFIED_TRANSACTION_SCHEDULER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected FRFCFS UnifiedFIFOXactionScheduler, with an eight-entry non-flow,
// non-pipe queue and one through 32 outstanding writes. All state advances only
// on targetFire. RR lastGrant and queue payload are deliberately unreset,
// matching Chisel RRArbiter/Queue; pointer/count reset does not suppress memory
// writes. Ports in order: clock,reset,targetFire;
// arValid,arReady,arId:4,arLen:8, arAddr:35;
// awValid,awReady,awId:4,awLen:8,awAddr:35; wValid,wLast,wReady;
// pendingWReq,pendingAWReq (maxWrites+1 bits); nextReady,nextValid,nextId:4,
// nextLen:8,nextIsWrite,nextAddr:35. Remaining ports are UInt<1>.
// Materializes GGFASEDUnifiedTransactionScheduler<capacity>. Unsupported
// capacities or symbol collisions fail atomically, preserving circuit and
// result.
mlir::LogicalResult materializeFASEDUnifiedTransactionScheduler(
    circt::firrtl::CircuitOp circuit, unsigned maxWrites,
    circt::firrtl::FModuleOp &result, std::string &error);
}
#endif
