// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_TIMING_MONITOR_H
#define GOLDENGATE_FASED_TIMING_MONITOR_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected FirstReadyFCFS parent statistics and four-rank power monitor. State
// and synchronous reset advance only on targetFire. Supports the actual 10/20
// constructor limits; histogram bounds are 0,2,4,8 and the fifth bin stays zero
// (the Scala implementation zips five registers with only four bounds).
// Does not impose request admission or implement functional release/MMIO.
// Requires the matching GGFASEDTimingMonitor10/20 symbol to be free. Failure
// leaves both the circuit and result unchanged.
mlir::LogicalResult
materializeFASEDTimingMonitor(circt::firrtl::CircuitOp circuit,
                              unsigned maxFlight,
                              circt::firrtl::FModuleOp &result,
                              std::string &error);
} // namespace goldengate
#endif
