// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// HostClockWiring.execute: exactly one source, optional sinks, Clock references.
// Connect local sinks or route a unique source through the instance hierarchy
// to every instance of pathless sinks. Consume source/sink annotations even
// when no sinks exist (without resolving the unused source target).
// Local instance Clock inputs, including AutoILA's wrapper clock, are sinks
// in the containing module; their existing placeholder drivers are replaced.
// retainSource selects Scala HostClockWiring.apply semantics: prepend the
// original source annotation so a later invocation can wire additional sinks.
// Reject unsupported targets or hierarchy before mutating the circuit.
mlir::LogicalResult wireHostClock(circt::firrtl::CircuitOp circuit,
                                  unsigned &wired, std::string &error,
                                  bool retainSource = false);
} // namespace goldengate
