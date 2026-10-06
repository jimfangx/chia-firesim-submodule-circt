// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_HISTOGRAMS_H
#define GOLDENGATE_FASED_HISTOGRAMS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize eight UInt32 counters and ten read-only lanes at bytes 16-52.
// No top/annotation changes or instances. Recorded bins 0,2,4,8 increment on
// targetFire; the fifth lanes stay zero per the Scala zip rule. Host reset
// masks assertions; modelReset resets state only when targetFire is high.
// On duplicate symbol failure, leave IR and result unchanged.
mlir::LogicalResult materializeFASEDHistograms(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &result, std::string &error);
// Attach that unused bank to the recorded statistics wrapper. Validate exact
// ports, ownership, constructor and observation boundary before any mutation;
// preserve drivers and transfer copied top targets. Observe pending AW, not W.
mlir::LogicalResult attachFASEDHistograms(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined compatibility entry point: preflight before materialization.
mlir::LogicalResult addFASEDHistograms(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
