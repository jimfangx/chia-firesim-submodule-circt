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
// directly promoted instances of each SRAM definition. Transforms local
// channel hardware once and rewires every instance. Other models stay at the prepared
// boundary; inter-model transport and SRAM timing-model replacement are later
// steps. Rebuilds hierarchy between port rewrites. Commits a verified clone,
// so unsupported instances/metadata leave the original circuit unchanged.
mlir::LogicalResult rewriteSRAMFAME(
    circt::firrtl::CircuitOp circuit, unsigned &rewritten, std::string &error);
// Construct both parent and SRAM FAME definitions at the SFC FAMETransform
// boundary, before SimWrapper queues. Shares the transport preconditions and
// transaction, but leaves channel ports exposed for boundary comparisons.
// Direct scalar top passthroughs become whole Decoupled connections, with
// endpoint targets and wrapper payload identities transferred to bits. Require
// unique PipeChannel endpoints, exclusive ground wiring and unused new names;
// unsupported fanout/metadata fails before committing the circuit clone.
mlir::LogicalResult rewriteSRAMParentFAME(
    circt::firrtl::CircuitOp circuit, unsigned &rewritten, std::string &error);
// Construct the parent clock hub and directly promoted SRAM FAME models, then
// join scalar PipeChannel endpoints with host-clocked queues. Requires one hub,
// scalar integer data groups and complete model coverage; no ready-valid pairs.
// Preserves retained inner endpoint/clock identities through wrapper activation.
// Commits only a verified clone. Optional timing-model replacement is separate.
mlir::LogicalResult rewriteSRAMPipeTransport(
    circt::firrtl::CircuitOp circuit, unsigned &rewritten, std::string &error);
}
