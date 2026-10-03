// See LICENSE for license details.
#pragma once
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// Lower a clone of the transformed simulator through CIRCT's standard
// FIRRTL -> HW -> SV -> ExportVerilog pipeline. Retained Golden Gate JSON
// stays on the source IR; attached CIRCT annotations remain on the clone.
// Retained inline blackbox sources are attached to clone external modules and
// emitted beside the RTL with CIRCT's blackbox resource list for FireSim.
// A failed lowering never replaces an existing RTL output.
mlir::LogicalResult emitSimulatorRTL(mlir::ModuleOp source,
                                    llvm::StringRef inputFilename,
                                    llvm::StringRef outputFilename,
                                    std::string &error);
} // namespace goldengate
