// See LICENSE for license details.
#pragma once
#include "goldengate/CombDependencyAnalysis.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
struct SRAMModelDependencies {
  circt::firrtl::FModuleOp module;
  std::vector<LocalChannelDependency> outputs;
};
// Prepare the optional SRAM model graph through InferModelPorts.
mlir::LogicalResult prepareSRAMModelChannels(
    mlir::ModuleOp root, circt::firrtl::CircuitOp circuit,
    unsigned &wrapped, unsigned &promoted, std::string &error);

// Bind the prepared graph and derive the dependencies consumed by FAME's
// output-valid rules. Requires normalized ground FIRRTL and inferred local
// groups. No mutation; unresolved paths fail instead of implying independence.
std::optional<llvm::SmallVector<SRAMModelDependencies>>
analyzeSRAMModelDependencies(circt::firrtl::CircuitOp circuit,
                             std::string &error);
}
