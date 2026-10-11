// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
/// Wrap one supported CPU platform boundary. Requires explicit FAME host
/// identities; does not infer or fabricate annotations from port names.
mlir::LogicalResult attachFPGATopShell(circt::firrtl::CircuitOp circuit,
                                     llvm::StringRef inputBoundary,
                                     std::string &error);
mlir::LogicalResult assembleFPGATopShell(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
