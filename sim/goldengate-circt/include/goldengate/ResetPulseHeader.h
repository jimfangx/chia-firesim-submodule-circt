// See LICENSE for license details.
#ifndef GOLDENGATE_RESETPULSEHEADER_H
#define GOLDENGATE_RESETPULSEHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append ResetPulseBridge driver collateral after validating the live MMIO
// allocation, pulse-width/polarity domain and matching register read/write slots.
mlir::LogicalResult prepareResetPulseHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
