// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADDISPATCH_H
#define GOLDENGATE_CONTROLREADDISPATCH_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter AR DecoupledHelper gates, readiness priority and payload broadcast.
// Requires: uninstantiated widget write wrapper, retained rawAnnotations,
// ordered unique controlRegions (1..63), matching decoder route/target widths,
// and a controlWriteBindings subset with exact U250 b/ar/r bundles.
// Consumes: decoder address and error request boundaries; mapped widget AR IO.
// Produces: controlReadBindings and helper controlRegions; tracker enqueue IO.
// Mutates: adds helper/wrapper and explicitly transfers copied b/r leaf targets;
// whole mapped bundles and consumed AR targets retain their inner identity.
// Analyses: decoder catalog and binding metadata; no cached analyses preserved.
// Output: count-bit route, ceil(log2(count+1))-bit target including error index;
// requests and tracker enqueue accept together under independent backpressure.
mlir::LogicalResult addControlReadDispatch(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
