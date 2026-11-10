// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_COLLAPSING_BUFFER_H
#define GOLDENGATE_FASED_COLLAPSING_BUFFER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Eight-entry selected FRFCFS request buffer (Util.scala CollapsingBuffer).
// Ports: clock/reset/targetFire, enqValid/enqReady/enqBits, entries/updates,
// programmableDepth: UInt<4>. Each entry is valid plus bits containing
// id:4,len:8,isWrite:1,rowAddr:26,bankAddrOH:8,bankAddr:3,rankAddrOH:4,
// rankAddr:2,isReady:1,mayPRE:1. The request fields flatten xaction metadata.
// Updates must be driven to entries by default, as in the Scala companion.
// State and synchronous valid reset advance only on targetFire; bits are
// intentionally unreset. Dynamic depth retains the oracle's reversed UIntToOH.
// Requires a free helper symbol; failure leaves the circuit/result unchanged.
mlir::LogicalResult materializeFASEDCollapsingBuffer(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
