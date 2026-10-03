// See LICENSE for license details.
#ifndef GOLDENGATE_PEEKPOKEHEADER_H
#define GOLDENGATE_PEEKPOKEHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Append PeekPokeBridge driver collateral after validating the live MMIO
// allocation, constructor port maps and matching register read/write slots.
mlir::LogicalResult preparePeekPokeHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
