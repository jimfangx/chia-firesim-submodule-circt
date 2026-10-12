// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
/// Materialize the zero-sink engine and bind it to an uninstantiated boundary.
/// Reject existing AW/W/B or incoming stream ports before any mutation.
mlir::LogicalResult materializeEmptyCPUStreamWrite(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
/// Bind an existing zero-sink native engine to a fresh AW/W/B boundary.
/// All validation precedes mutation; prior module and annotation identities remain.
mlir::LogicalResult attachEmptyCPUStreamWrite(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp helper, llvm::StringRef wrapperName, std::string &error);
}
