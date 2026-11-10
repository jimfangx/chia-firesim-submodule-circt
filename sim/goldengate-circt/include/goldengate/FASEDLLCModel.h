// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_LLC_MODEL_H
#define GOLDENGATE_FASED_LLC_MODEL_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Exact selected LLC timing model: eight ways, 4096 sets, eight MSHRs, 35-bit
// addresses, 4-bit IDs and 64-bit data beats. Carries address/length metadata;
// data is tracked by the separate functional memory model. Synchronous metadata
// RAM relies on programming-time initialization; reset does not clear it.
// Materializes GGFASEDLLCModel and its metadata/MSHR helpers atomically.
mlir::LogicalResult materializeFASEDLLCModel(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
