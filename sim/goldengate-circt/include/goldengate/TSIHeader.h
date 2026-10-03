// See LICENSE for license details.
#ifndef GOLDENGATE_TSIHEADER_H
#define GOLDENGATE_TSIHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append TSIBridge driver collateral after validating the live MMIO
// allocation, nine-register ABI and host memory offset and matching register read/write slots.
mlir::LogicalResult prepareTSIHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
