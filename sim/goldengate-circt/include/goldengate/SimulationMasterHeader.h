// See LICENSE for license details.
#ifndef GOLDENGATE_SIMULATIONMASTERHEADER_H
#define GOLDENGATE_SIMULATIONMASTERHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append Master.scala/Widget.genConstructor collateral to the existing driver
// header. Resolve the live bank, binding and allocation through InstanceGraph;
// reject unsupported/malformed boundaries before changing any annotation.
mlir::LogicalResult prepareSimulationMasterHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
