// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_RESPONSE_ERRORS_H
#define GOLDENGATE_FASED_RESPONSE_ERRORS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Create two host-clocked reset-to-zero error registers and their read-only
// registry at global bytes 76/80. Preserve top/ports/annotations; leave unused.
mlir::LogicalResult materializeFASEDResponseErrors(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach that bank once the exact host response bundles exist; observe the
// accepted R/B handshakes, preserve SFC capture behavior, and retarget identities.
mlir::LogicalResult attachFASEDResponseErrors(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);
// Combined compatibility entry point; rejected boundaries leave IR unchanged.
mlir::LogicalResult addFASEDResponseErrors(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
