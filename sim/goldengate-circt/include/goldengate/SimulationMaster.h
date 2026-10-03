// See LICENSE for license details.
#ifndef GOLDENGATE_SIMULATIONMASTER_H
#define GOLDENGATE_SIMULATIONMASTER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult addSimulationMasterBank(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
