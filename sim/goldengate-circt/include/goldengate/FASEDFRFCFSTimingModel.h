// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_TIMING_MODEL_H
#define GOLDENGATE_FASED_FRFCFS_TIMING_MODEL_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected FirstReadyFCFS timing model at the target request and functional
// egress boundary. Composes LLC, DRAM controller, AXI response release, target
// pending counts, histograms and power counters. Functional target memory,
// MMIO/host token plumbing and platform integration remain external. maxFlight
// is 10 for Shuttle and 20 for MegaBoom/Saturn. Requires free dependency names;
// rejection preserves the circuit and output handle.
mlir::LogicalResult
materializeFASEDFRFCFSTimingModel(circt::firrtl::CircuitOp circuit,
                                  unsigned maxFlight,
                                  circt::firrtl::FModuleOp &result,
                                  std::string &error);
} // namespace goldengate
#endif
