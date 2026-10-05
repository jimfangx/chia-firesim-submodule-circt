// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADARBITER_H
#define GOLDENGATE_CONTROLREADARBITER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Required input invariants: an uninstantiated GGControlReadTrackerWrapper,
// retained rawAnnotations, an ordered unique controlRegions catalog, matching
// controlReadBindings and exact response/retirement port types and directions.
// Annotations consumed: none. Preserved/transferred: raw target identities;
// retained B fields move outward, removed R fields stay on the inner module.
// Analyses required: decoded control regions and read widget bindings.
// Analyses preserved: catalog and read bindings; top target paths are updated.
// IR mutations: wrap the top, split B/R ports, create FIRRTL arbitration state
// and wire accepted final beats to each unbound slave's tracker retirement.
// Output invariants: one selected R source, burst lock held across stalls/gaps,
// host reset clears cursor/lock, mapped/error R ports are consumed internally.
// NastiRouter's HellaPeekingArbiter with one source per decoded region (1..63)
// plus the error responder. Accepted last beats release the burst lock and
// retire the existing AR tracker slots.
mlir::LogicalResult addControlReadArbiter(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
