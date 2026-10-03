// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Port of GlobalResetConditionWiring for local lowered UInt<1> references.
// Supports sinks in the source module, or a circuit-top source routed downward
// to all instances of pathless sink modules. Adds one input per route module.
// Rejects unreachable uses, nested source ownership and conditional drivers
// before mutation. Public annotations share their internal shadow semantics.
// With either side absent, consume annotations without changing hardware.
mlir::LogicalResult wireGlobalReset(circt::firrtl::CircuitOp circuit,
                                    unsigned &wired, std::string &error);
} // namespace goldengate
