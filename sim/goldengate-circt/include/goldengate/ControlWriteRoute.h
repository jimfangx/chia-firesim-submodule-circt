// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITEROUTE_H
#define GOLDENGATE_CONTROLWRITEROUTE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter's AW route queue and AW/W DecoupledHelper gates. One route
// per accepted AW, popped only on accepted W.last. One entry per decoded slave
// (1..63), no flow/pipe; reset flushes occupancy while accepted writes still
// update unreset RAM.
// Requires: uninstantiated GGControlDecodeWrapper, host clock/reset, retained
// rawAnnotations and GGControlAddressDecode's 1..63-entry controlRegions.
// Decoded AW route width must equal the region count.
// Consumes: no annotations. Produces: queue depth/flow/pipe attributes.
// Mutates: adds helper and wrapper modules; transfers copied top-port targets
// and circuit prefixes while retaining internal target identities.
// Analyses required/preserved: none; callers must rebuild hierarchy analyses.
// Output: GGControlWriteRouteWrapper with ctrl_write_route_* handshake and
// route boundaries. Selected-slave/tracker readiness and payload wiring are
// bound by later passes.
mlir::LogicalResult addControlWriteRoute(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
#endif
