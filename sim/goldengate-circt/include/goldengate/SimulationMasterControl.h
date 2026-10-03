// See LICENSE for license details.
#ifndef GOLDENGATE_SIMULATIONMASTERCONTROL_H
#define GOLDENGATE_SIMULATIONMASTERCONTROL_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult bindSimulationMasterControl(circt::firrtl::CircuitOp circuit,
                                              std::string &error);
// Bind TSI AW/W/AR requests and B/R responses to the recorded control slave 3.
mlir::LogicalResult bindTSIBridgeControl(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
// Bind BlockDev AW/W/AR requests and B/R responses to control slave 0,
// allocated at [0x0, 0x80) in the recorded U250 widget catalog.
mlir::LogicalResult bindBlockDevBridgeControl(circt::firrtl::CircuitOp circuit,
                                            std::string &error);
// Bind FASED AW/W/AR requests and B/R responses to control slave 1,
// allocated at [0x80, 0x100) in the recorded U250 widget catalog.
mlir::LogicalResult bindFASEDBridgeControl(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Assemble the host control master after all eleven widgets are bound.
mlir::LogicalResult bindControlMaster(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
}
#endif
