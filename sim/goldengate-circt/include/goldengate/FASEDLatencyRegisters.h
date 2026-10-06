// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_LATENCY_REGISTERS_H
#define GOLDENGATE_FASED_LATENCY_REGISTERS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Create two host-clocked 32-bit reset-to-30 registers and their MCR registry.
// Requires a free bank symbol; preserves top/ports/annotations and leaves result
// unchanged on failure. No annotation classes or cached analyses consumed.
mlir::LogicalResult materializeFASEDLatencyRegisters(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach the same uninstantiated four-port bank to the recorded ten-flight
// request-limit wrapper. Boundary preflight precedes mutation; copied-port
// targets transfer while consumed latency targets remain on the inner module.
mlir::LogicalResult attachFASEDLatencyRegisters(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);

// Decode lanes 0/1 are the write/read latency words at byte offsets 0/4.
mlir::LogicalResult addFASEDLatencyRegisters(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
