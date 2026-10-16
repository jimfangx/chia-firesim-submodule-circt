// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_HISTOGRAMS_H
#define GOLDENGATE_FASED_HISTOGRAMS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize eight UInt32 counters and ten read-only lanes at bytes 16-52.
// No top/annotation changes or instances. Recorded bins 0,2,4,8 increment on
// targetFire; the fifth lanes stay zero per the Scala zip rule. Host reset
// masks nonfatal read-only write diagnostics; modelReset resets state only
// when targetFire is high. Diagnostics remain on the host clock.
// Requires positive retained maxFlight (mapped engine or unique early BridgeIO).
// Pending read/AW inputs use log2Ceil(min(16,maxFlight)+1) bits, matching the
// admission counters. Missing/malformed capacity or duplicate symbols leave
// IR and result unchanged.
mlir::LogicalResult materializeFASEDHistograms(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &result, std::string &error);
// Attach that unused bank to the constructor-sized statistics wrapper. Validate exact
// ports, ownership, constructor and observation boundary before any mutation;
// preserve drivers and transfer copied top targets. Observe pending AW, not W.
mlir::LogicalResult attachFASEDHistograms(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined compatibility entry point: preflight before materialization.
mlir::LogicalResult addFASEDHistograms(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
