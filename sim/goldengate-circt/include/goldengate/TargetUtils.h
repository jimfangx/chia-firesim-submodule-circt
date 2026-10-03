// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <cstdint>
#include <optional>
#include <string>

namespace goldengate {
// A resolved annotation target refers to CIRCT operations, a port index, and
// (for aggregate port members) CIRCT's field ID.  Textual SFC targets are used
// only at the import boundary.
struct GGTarget {
  circt::firrtl::CircuitOp circuit;
  circt::firrtl::FModuleLike module;
  std::optional<unsigned> port;
  std::optional<uint64_t> fieldID;
  // SFC-compatible ground name derived from the FIRRTL port path. Empty for
  // module targets; the actual CIRCT port transfer still occurs at lowering.
  std::string groundPortName;
};

// Handles local module and port targets, including aggregate port fields.
// Hierarchical instance paths and internal references are deliberately not
// interpreted as ports.
std::optional<GGTarget> resolveAnnotationTarget(circt::firrtl::CircuitOp circuit,
                                                llvm::StringRef spelling,
                                                std::string &error);

// Resolve a local reference to the CIRCT declaration operation.  This is
// separate from port resolution so consumers requiring a module or port do
// not accidentally accept an internal reference.
mlir::Operation *resolveInternalAnnotationTarget(
    circt::firrtl::CircuitOp circuit, llvm::StringRef spelling,
    std::string &error);

// Typed local selectors for single-result declarations, kept separate from
// the root-only resolver used by consumers of already lowered values.
struct GGInternalTarget {
  circt::firrtl::FModuleLike module;
  mlir::Operation *declaration;
  uint64_t fieldID;
  circt::firrtl::FIRRTLBaseType type;
};
std::optional<GGInternalTarget> resolveInternalFieldTarget(
    circt::firrtl::CircuitOp circuit, llvm::StringRef spelling,
    std::string &error);
} // namespace goldengate
