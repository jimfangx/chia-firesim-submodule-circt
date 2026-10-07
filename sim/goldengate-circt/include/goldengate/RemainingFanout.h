// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Requires post-FAME source renaming: each PipeChannel source resolves to a
// live local output port/field. Source sequences are ordered identities, not
// unordered sets. Consumes no annotations; appends ChannelFanout annotations
// for sequences shared by more than one distinct channel name, preserving
// first-source and first-name insertion order (Scala AddRemainingFanoutAnnotations).
// Existing annotations remain unchanged, including bridge-sourced fanouts.
// Run once after source deduplication, before simulator queue construction:
// like Scala, repeating this step appends the groups again. Preflight all
// source identities before mutation. Mutates only rawAnnotations, requiring
// no cached analyses and preserving module, port, hierarchy and target analyses.
mlir::LogicalResult addRemainingFanoutAnnotations(
    circt::firrtl::CircuitOp circuit, std::string &error);
} // namespace goldengate
