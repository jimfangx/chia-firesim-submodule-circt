// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/IR/BuiltinOps.h"
#include <string>

namespace goldengate {
// Prepare the optional SRAM model graph through InferModelPorts.
mlir::LogicalResult prepareSRAMModelChannels(
    mlir::ModuleOp root, circt::firrtl::CircuitOp circuit,
    unsigned &wrapped, unsigned &promoted, std::string &error);
}
