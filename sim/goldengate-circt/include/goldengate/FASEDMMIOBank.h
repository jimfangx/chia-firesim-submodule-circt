// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_MMIO_BANK_H
#define GOLDENGATE_FASED_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Join the six recorded register fragments into global MCR words 0-20.
mlir::LogicalResult addFASEDMMIOBank(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
}
#endif
