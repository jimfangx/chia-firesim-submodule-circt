// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Scala TriggerWiring consumes source annotations without changing the circuit
// when there are no trigger sinks. Reject the hardware-generating case until
// its counter and sink wiring semantics are implemented in CIRCT.
mlir::LogicalResult consumeUnobservedTriggerSources(
    circt::firrtl::CircuitOp circuit, unsigned &consumed,
    std::string &error);
} // namespace goldengate
