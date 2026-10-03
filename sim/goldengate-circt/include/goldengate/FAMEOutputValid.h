// See LICENSE for license details.
#pragma once

#include "goldengate/CombDependencyAnalysis.h"
#include <string>

namespace goldengate {
// Required input invariants: model ports already use FAME decoupled bundles;
// each output has one valid connect and a host-clocked fired register.
// Annotations consumed/produced: none.
// IR mutations: replace output-valid connect sources with FIRRTL expressions.
// Analysis required: local combinational input-channel dependencies.
mlir::LogicalResult rewriteFAMEOutputValids(
    circt::firrtl::FModuleOp module,
    llvm::ArrayRef<LocalChannelDependency> dependencies, std::string &error);
} // namespace goldengate
