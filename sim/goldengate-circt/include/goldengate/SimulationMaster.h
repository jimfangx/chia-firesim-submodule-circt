// See LICENSE for license details.
#ifndef GOLDENGATE_SIMULATIONMASTER_H
#define GOLDENGATE_SIMULATIONMASTER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Construct the standalone Master.scala register bank before global address
// allocation. Requires an unused bank symbol; creates FIRRTL operations and
// goldengate.mmioRegisters, consumes/produces no annotations, preserves circuit
// identity/top ports. No analyses required/preserved. Result is unchanged on failure.
mlir::LogicalResult materializeSimulationMasterBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach that uninstantiated bank exactly once to the post-control-tracker
// top. Require exact clock/reset/three-word MCR ports and retained annotations;
// transfer copied top-port targets to GGSimulationMasterWrapper. No annotation
// classes consumed/produced; no analyses required/preserved. Preflight is atomic.
mlir::LogicalResult attachSimulationMasterBank(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined materialization/attachment with the same atomic preflight.
mlir::LogicalResult addSimulationMasterBank(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
