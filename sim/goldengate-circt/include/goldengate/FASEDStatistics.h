// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_STATISTICS_H
#define GOLDENGATE_FASED_STATISTICS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize the read-only W/R beat and AW/AR transaction bank before control
// allocation. Local lanes 0-3 retain global words 14-17 (byte offsets 56-68).
// Counters advance/reset only on targetFire; hostReset suppresses assertions.
// Top identity, ports and retained annotations are preserved; result is assigned
// only on success. Requires a free GGFASEDStatistics symbol.
mlir::LogicalResult materializeFASEDStatistics(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &result, std::string &error);
// Attach that exact nine-port, uninstantiated bank after response mapping.
// Validate the complete response hierarchy and bank before mutating any ports.
mlir::LogicalResult attachFASEDStatistics(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Compatibility API: preflight, materialize and attach atomically on rejection.
mlir::LogicalResult addFASEDStatistics(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
