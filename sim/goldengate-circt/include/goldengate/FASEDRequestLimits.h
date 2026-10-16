// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_REQUEST_LIMITS_H
#define GOLDENGATE_FASED_REQUEST_LIMITS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Shared constructor profile for the MMIO maxima and pending read/AW/W counters.
// Uses the mapped engine when present, otherwise the unique early BridgeIO key.
// Rejects missing/malformed maxFlight before callers mutate IR.
mlir::LogicalResult getFASEDRequestLimitProfile(circt::firrtl::CircuitOp circuit,
    unsigned &limit, unsigned &width, std::string &error);
// Create two host-clocked 32-bit registers resetting to min(16,maxFlight).
// Their admission outputs have log2Ceil(min(16,maxFlight)+1) bits.
// Requires a positive retained maxFlight and free bank symbol; preserves top/ports/annotations and leaves result
// unchanged on failure. No annotation classes or cached analyses consumed.
mlir::LogicalResult materializeFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach the same uninstantiated four-port bank to the constructor-sized
// read-admission wrapper. Boundary preflight precedes mutation; copied-port
// targets transfer while consumed admission targets remain on the inner module.
mlir::LogicalResult attachFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);

// Decode lanes 0/1 are the write/read maximum words at global byte offsets 8/12.
mlir::LogicalResult addFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
