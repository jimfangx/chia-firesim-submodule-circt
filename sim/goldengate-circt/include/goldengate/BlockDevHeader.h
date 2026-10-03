// See LICENSE for license details.
#ifndef GOLDENGATE_BLOCKDEVHEADER_H
#define GOLDENGATE_BLOCKDEVHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append BlockDevBridge driver collateral after validating the live MMIO
// allocation, 26-register ABI, tracker count and live latency width.
mlir::LogicalResult prepareBlockDevHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
