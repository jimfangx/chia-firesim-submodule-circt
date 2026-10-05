// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITEARBITER_H
#define GOLDENGATE_CONTROLWRITEARBITER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter RRArbiter and accepted B response signals. The decoder catalog
// supplies 1..63 normal sources; the error source occupies its final index.
// Read bindings identify consumed widget bundles by region name and index.
mlir::LogicalResult addControlWriteArbiter(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
