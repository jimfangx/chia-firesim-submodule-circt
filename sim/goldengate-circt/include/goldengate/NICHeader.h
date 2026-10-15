// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// SimpleNICBridgeModule.genHeader for the implemented normal-mode NIC. Requires
// the validated CPU stream header. Derives MMIO addresses and stream indices
// from live FIRRTL allocations; changes only the sole .const.h OutputFile.
mlir::LogicalResult prepareNICHeader(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
}
