// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// Serialize compiler metadata with the same validated JSON attribute encoding
// as retained annotations. Unsupported values fail before opening the file.
mlir::LogicalResult emitJSONAttribute(mlir::Attribute value,
                                     llvm::StringRef path,
                                     std::string &error);
// Emit the annotation attributes retained and retargeted by CIRCT's FAME
// rewrite. This is the post-FAME annotation boundary consumed by later passes.
mlir::LogicalResult emitFAMEAnnotations(circt::firrtl::CircuitOp circuit,
                                         llvm::StringRef path,
                                         std::string &error);
mlir::LogicalResult emitAllAnnotations(circt::firrtl::CircuitOp circuit,
                                       llvm::StringRef path,
                                       std::string &error);
// GoldenGateFileEmission: append fileSuffix to the configured output base
// and write body bytes verbatim. Validate all destinations before writing.
mlir::LogicalResult emitOutputFiles(circt::firrtl::CircuitOp circuit,
                                    llvm::StringRef directory,
                                    llvm::StringRef baseFilename,
                                    std::string &error);
} // namespace goldengate
