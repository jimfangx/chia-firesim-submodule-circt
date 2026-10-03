// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITEROUTE_H
#define GOLDENGATE_CONTROLWRITEROUTE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter's U250 AW route queue and AW/W DecoupledHelper gates. One route
// per accepted AW, popped only on accepted W.last. Eleven entries, no flow/pipe;
// reset flushes occupancy but accepted writes still update unreset RAM.
// Input: uninstantiated GGControlDecodeWrapper, retained annotations, host
// clock/reset and decoded eleven-bit AW route. Copied targets transfer. The
// ctrl_write_route_* boundary exposes selected-slave and response-tracker
// readiness; their implementation and request payload wiring follow later.
mlir::LogicalResult addControlWriteRoute(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
#endif
