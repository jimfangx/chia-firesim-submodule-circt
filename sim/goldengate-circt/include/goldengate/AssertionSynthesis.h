// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// After LowerTypes/ExpandWhens, export failing target assertions through every
// nonexcluded instance, grouped by resolved top clock. Preserve successful stops,
// assumptions and covers. Constructors, source channels and reset sinks commit
// atomically with the target RTL; disabled operation is handled by the caller.
mlir::LogicalResult synthesizeAssertions(circt::firrtl::CircuitOp circuit,
                                        unsigned &count, std::string &error);
}
