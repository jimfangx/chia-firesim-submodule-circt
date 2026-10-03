// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"

namespace goldengate {
// SFC Golden Gate interprets target Reset and AsyncReset as synchronous Bool
// resets. Run after target type lowering and before FAME gates target clocks.
void coerceAsyncToSyncReset(circt::firrtl::CircuitOp circuit);
} // namespace goldengate
