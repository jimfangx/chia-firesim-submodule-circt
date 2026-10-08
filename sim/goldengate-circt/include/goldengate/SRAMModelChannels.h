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

// Construct FAME's virtual clock hardware on the prepared SRAM definitions
// and update every promoted instance. Data ports remain scalar, and the
// finishing wire is reserved for the subsequent channel FSM rewrite.
// Requires one target clock and no model-local channel clock associations.
mlir::LogicalResult rewriteSRAMVirtualClocks(
    circt::firrtl::CircuitOp circuit, unsigned &rewritten, std::string &error);

// Channelize prepared scalar SRAM data ports, retarget their annotations to
// Decoupled bits, and construct the virtual-clock FAME channel FSM. Requires
// one promoted instance per SRAM definition. Other models stay at the prepared
// boundary; inter-model transport and SRAM timing-model replacement are later
// steps. Rebuilds hierarchy between port rewrites. Commits a verified clone,
// so unsupported instances/metadata leave the original circuit unchanged.
mlir::LogicalResult rewriteSRAMFAME(
    circt::firrtl::CircuitOp circuit, unsigned &rewritten, std::string &error);
}
