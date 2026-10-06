// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FUNCTIONAL_MODEL_REGISTER_H
#define GOLDENGATE_FASED_FUNCTIONAL_MODEL_REGISTER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Create the host-clocked 32-bit reset-to-zero register and global-word-18 registry.
// Requires a free bank symbol; preserves top/ports/annotations and leaves bank
// unchanged on failure. No annotation classes or cached analyses consumed.
mlir::LogicalResult materializeFASEDFunctionalModelRegister(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp &bank, std::string &error);
// Attach the same uninstantiated four-port bank to the ten-flight latency wrapper.
// Boundary preflight precedes mutation; copied-port targets transfer while the
// consumed ingress-relaxation target remains on the inner module.
mlir::LogicalResult attachFASEDFunctionalModelRegister(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp bank, std::string &error);

// Decoded lane zero is relaxFunctionalModel at byte offset 72.
mlir::LogicalResult addFASEDFunctionalModelRegister(circt::firrtl::CircuitOp circuit,
                                                 std::string &error);
}
#endif
