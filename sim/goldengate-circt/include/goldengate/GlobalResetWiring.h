// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Port of GlobalResetConditionWiring for lowered ground references in one
// module. Public targetutils annotations have the same semantics as their
// internal shadow annotations. Cross-module wiring is rejected before mutation.
// With either side absent, consume both annotation classes without changing IR.
mlir::LogicalResult wireLocalGlobalReset(circt::firrtl::CircuitOp circuit,
                                         unsigned &wired, std::string &error);
} // namespace goldengate
