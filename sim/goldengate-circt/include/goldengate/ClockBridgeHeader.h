// See LICENSE for license details.
#ifndef GOLDENGATE_CLOCKBRIDGEHEADER_H
#define GOLDENGATE_CLOCKBRIDGEHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append ClockBridge.scala/Widget.genConstructor collateral to .const.h after
// validating the live allocation, bindings and typed snapshot/latch operations.
mlir::LogicalResult prepareClockBridgeHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
