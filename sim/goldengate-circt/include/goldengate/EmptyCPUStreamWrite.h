// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
/// Bind an existing zero-sink native engine to a fresh AW/W/B boundary.
/// All validation precedes mutation; prior module and annotation identities remain.
mlir::LogicalResult attachEmptyCPUStreamWrite(circt::firrtl::CircuitOp circuit,
    circt::firrtl::FModuleOp helper, llvm::StringRef wrapperName, std::string &error);
}
