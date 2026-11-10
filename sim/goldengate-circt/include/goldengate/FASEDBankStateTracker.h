// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_BANK_STATE_TRACKER_H
#define GOLDENGATE_FASED_BANK_STATE_TRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Materialize the selected FRFCFS16GBQuadRank DDR3 bank timing primitive.
// Row addresses are 26 bits (DramOrganizationParams(8,4,1<<34,lineBits=8)).
// All state and synchronous reset advance only on targetFire. Open-row storage
// is intentionally unreset. Command legality assertions retain the Scala oracle.
// The caller must connect rank legality, refresh and scheduler policy separately.
// Requires a free helper symbol; failure leaves the circuit and result unchanged.
mlir::LogicalResult materializeFASEDBankStateTracker(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
