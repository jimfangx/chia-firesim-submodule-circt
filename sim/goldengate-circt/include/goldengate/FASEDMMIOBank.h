// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_MMIO_BANK_H
#define GOLDENGATE_FASED_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Join the six recorded register fragments into global MCR words 0-20.
// Registry row order defines local lanes; aligned offsets define global lanes.
// Accept every positive retained maxFlight supported by the default BaseConfig;
// capacity does not alter the UInt32 register lanes or their permissions.
// Validate the mapped constructor, registry and typed boundaries before mutation.
mlir::LogicalResult addFASEDMMIOBank(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
}
#endif
