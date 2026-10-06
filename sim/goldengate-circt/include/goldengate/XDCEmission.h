// See LICENSE for license details.
#pragma once
#include "goldengate/ChannelAnalysis.h"
#include <string>
namespace goldengate {
// FAMETransform's hub clock constraint, attached to the actual gate instance
// resolved by original clock identity. Legacy names are accepted only in
// entirely untagged boundaries; final XDC follows the instance's current name.
mlir::LogicalResult addFAMEClockConstraint(circt::firrtl::CircuitOp circuit,
                                   circt::firrtl::FModuleOp model,
                                   llvm::StringRef modelClockName,
                                   const RationalClockInfo &clock,
                                   std::string &error);
// WriteXDCFile: expand each root module through CIRCT's instance graph,
// format validated references, consume snippets/paths, produce both files.
// No module bodies change; all validation precedes annotation mutation.
mlir::LogicalResult prepareXDCOutput(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
} // namespace goldengate
