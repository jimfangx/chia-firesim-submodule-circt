// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_CONTROLLER_H
#define GOLDENGATE_FASED_FRFCFS_CONTROLLER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected FRFCFS timing controller, from raw LLC memory requests through
// transaction arbitration, programmable address mapping, command scheduling
// and dynamic-latency completions. Owns the fire-gated 64-bit target cycle.
// maxWrites is 10 (Shuttle) or 20 (MegaBoom/Saturn). Parent pending counts are
// target-side counts, not reconstructed from LLC traffic. Does not implement
// the LLC, functional memory model, response releaser, MMIO or power counters.
// A free dependency symbol set is required; failures are atomic.
mlir::LogicalResult materializeFASEDFRFCFSController(
    circt::firrtl::CircuitOp circuit, unsigned maxWrites,
    circt::firrtl::FModuleOp &result, std::string &error);
}
#endif
