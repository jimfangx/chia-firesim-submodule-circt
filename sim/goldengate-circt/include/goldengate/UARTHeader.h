// See LICENSE for license details.
#ifndef GOLDENGATE_UARTHEADER_H
#define GOLDENGATE_UARTHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append UARTBridge driver collateral after validating the live MMIO
// allocation, six-register ABI and matching register read/write slots.
mlir::LogicalResult prepareUARTHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
