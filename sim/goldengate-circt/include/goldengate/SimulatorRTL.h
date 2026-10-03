// See LICENSE for license details.
#pragma once
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// Match CIRCT's generated wide-memory initializer and share one 32-bit random
// draw across chunks and rows, as SFC's replicated initialization word does.
// Unknown loop shapes, guards, bounds and extra consumers remain unchanged.
unsigned normalizeMemoryInitialization(mlir::ModuleOp module);
// Narrow a part-select base only when an SV initialization loop's strict
// constant upper bound proves that every executed base fits. Loop induction
// types/bounds/steps stay unchanged, including their termination bit.
unsigned normalizeInitializationIndices(mlir::ModuleOp module);
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
