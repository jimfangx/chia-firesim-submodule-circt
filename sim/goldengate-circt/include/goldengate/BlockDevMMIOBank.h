// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEV_MMIO_BANK_H
#define GOLDENGATE_BLOCKDEV_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Construct the standalone 26-word register bank before control allocation.
// Requires a free bank symbol; creates FIRRTL/registry metadata, preserves top
// identity/ports/annotations and leaves result unchanged on failure.
// No annotation classes consumed/produced; no analyses required/preserved.
mlir::LogicalResult materializeBlockDevMMIOBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach exactly once to the one-tracker BlockDev queue boundary. Requires an
// uninstantiated ten-port bank in this circuit and exact top ports/annotations.
// Preflight precedes mutation; creates wrapper, transfers copied-port targets,
// retains inner target identity. No annotation classes or analyses consumed.
mlir::LogicalResult attachBlockDevMMIOBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined construction/attachment with atomic boundary preflight.
mlir::LogicalResult addBlockDevMMIOBank(circt::firrtl::CircuitOp circuit,
                                      std::string &error);
}
#endif
