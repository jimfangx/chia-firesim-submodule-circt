// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLERRORSLAVE_H
#define GOLDENGATE_CONTROLERRORSLAVE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Same endpoint for an explicitly selected, typed native control top.
mlir::LogicalResult addControlErrorSlave(circt::firrtl::CircuitOp circuit,
    unsigned addressBits, unsigned idBits, llvm::StringRef expectedTop,
    std::string &error);
// Materialize the junctions.nasti.scala NastiErrorSlave needed by the control
// router's unmapped-address branch. Read and write queues each have one entry;
// reads produce len+1 DECERR beats and writes drain through W.last before B.
// Input: uninstantiated GGLoadMemControlWrapper, retained annotations, exact
// host clock/reset. Copied targets transfer. Address routing remains pending;
// the new ctrl_error_* boundary exposes this endpoint for the router rewrite.
mlir::LogicalResult addControlErrorSlave(circt::firrtl::CircuitOp circuit,
                                       unsigned addressBits, unsigned idBits,
                                       std::string &error);
}
#endif
