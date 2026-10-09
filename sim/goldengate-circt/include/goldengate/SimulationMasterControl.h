// See LICENSE for license details.
#ifndef GOLDENGATE_SIMULATIONMASTERCONTROL_H
#define GOLDENGATE_SIMULATIONMASTERCONTROL_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/StringRef.h"
#include <string>
namespace goldengate {
// Bind each widget's AW/W/AR requests and B/R responses to the slave selected
// by its unique name in the ordered 1..63-region decoder catalog. Validate
// all row identities and nonoverlapping 25-bit bounds before IR mutation.
// AXI widths remain those of the supported U250 control interface. Copied
// targets transfer to the wrapper; internalized targets retain inner identity.
mlir::LogicalResult bindSimulationMasterControl(circt::firrtl::CircuitOp circuit,
                                              std::string &error);
mlir::LogicalResult bindTSIBridgeControl(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
mlir::LogicalResult bindBlockDevBridgeControl(circt::firrtl::CircuitOp circuit,
                                            std::string &error);
mlir::LogicalResult bindFASEDBridgeControl(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Assemble the host control master after the widget controls are bound.
mlir::LogicalResult bindControlMaster(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
// Explicit assembly boundary for selected hosts. Accepts only the complete
// widget-binding stage above or GGControlWriteTrackerWrapper. The caller
// must finish binding every selected slave's requests/responses first.
// Validate all U250 scalar fields and the full AR bundle before mutation;
// copied targets transfer and internalized targets retain module identity.
mlir::LogicalResult bindControlMaster(circt::firrtl::CircuitOp circuit,
                                    llvm::StringRef inputName,
                                    std::string &error);
}
#endif
