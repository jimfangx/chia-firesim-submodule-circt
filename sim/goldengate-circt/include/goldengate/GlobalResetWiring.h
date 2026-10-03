// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Port of GlobalResetConditionWiring for local lowered UInt<1> references.
// Supports local sinks and a uniquely instantiated source routed upward to the
// lowest common ancestor, then downward to every instance of pathless sinks.
// Adds one output on source ancestry and one input on sink branches.
// Rejects unreachable uses, ambiguous source ownership and conditional drivers
// before mutation. Public annotations share their internal shadow semantics.
// With either side absent, consume annotations without changing hardware.
mlir::LogicalResult wireGlobalReset(circt::firrtl::CircuitOp circuit,
                                    unsigned &wired, std::string &error);
} // namespace goldengate
