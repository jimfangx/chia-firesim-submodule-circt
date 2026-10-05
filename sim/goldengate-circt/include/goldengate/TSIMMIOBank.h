// See LICENSE for license details.
#ifndef GOLDENGATE_TSI_MMIO_BANK_H
#define GOLDENGATE_TSI_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Construct TSI's standalone nine sampled/data/pulse registers before address
// allocation. Requires a free bank symbol; creates FIRRTL and register metadata,
// preserves circuit identity/top ports/annotations, leaves result on failure.
// No analyses required/preserved; no annotation classes consumed/produced.
mlir::LogicalResult materializeTSIMMIOBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach that uninstantiated bank once to the TSI word-queue top. Preflight
// exact bank ports, clock/reset/queue/scheduler and retained annotations before
// mutation. Creates wrapper; copied port targets transfer, internal targets
// retain identity. No annotation classes consumed/produced or analyses used.
mlir::LogicalResult attachTSIMMIOBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined construction/attachment with atomic boundary preflight.
mlir::LogicalResult addTSIMMIOBank(circt::firrtl::CircuitOp circuit,
                                  std::string &error);
}
#endif
