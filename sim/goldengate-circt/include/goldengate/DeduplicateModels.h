// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>
namespace goldengate {
// Run the SFC post-ExtractModel dedup phase with retained-target renames.
// Atomic. Reject target schemas or identity changes whose mapping is unported.
mlir::LogicalResult deduplicateModels(circt::firrtl::CircuitOp circuit,
                                      unsigned &removed, std::string &error);
}
