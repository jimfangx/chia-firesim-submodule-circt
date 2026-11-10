// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_LLC_MSHR_TABLE_H
#define GOLDENGATE_FASED_LLC_MSHR_TABLE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Eight selected LLC MSHRs. Runtime enables are registered, reset to true,
// while flight flags reset to false and metadata remains unreset. All state
// and reset are gated by targetFire. Dynamic completion IDs use their low
// three bits, as in the immutable SFC lowering; completion clears override
// allocation. A full table's priority encoder returns index seven.
// Rejection leaves circuit and result unchanged.
mlir::LogicalResult materializeFASEDLLCMSHRTable(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
