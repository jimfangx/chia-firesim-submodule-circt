// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_REQUEST_LIMITS_H
#define GOLDENGATE_FASED_REQUEST_LIMITS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Create two host-clocked 32-bit reset-to-10 registers and their MCR registry.
// Requires a free bank symbol; preserves top/ports/annotations and leaves result
// unchanged on failure. No annotation classes or cached analyses consumed.
mlir::LogicalResult materializeFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach the same uninstantiated four-port bank to the recorded ten-flight
// read-admission wrapper. Boundary preflight precedes mutation; copied-port
// targets transfer while consumed admission targets remain on the inner module.
mlir::LogicalResult attachFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);

// Decode lanes 0/1 are the write/read maximum words at global byte offsets 8/12.
mlir::LogicalResult addFASEDRequestLimits(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
