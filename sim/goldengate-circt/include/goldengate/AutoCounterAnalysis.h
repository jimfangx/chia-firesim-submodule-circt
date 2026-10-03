// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/IR/Value.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
struct AutoCounterEvent {
  circt::firrtl::FModuleOp module;
  mlir::Value event;
  mlir::Value clock;
  mlir::Value reset;
  std::string target;
  std::string clockTarget;
  std::string resetTarget;
  std::string label;
  unsigned annotationIndex;
};

// Resolve retained public and internal AutoCounter targets to FIRRTL SSA values. The values are
// the operands needed by the later reset gating and counter synthesis passes.
mlir::LogicalResult analyzeAutoCounterEvents(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<AutoCounterEvent> &events, std::string &error);

// AutoCounterTransform selection: manual events are always included; generated
// covers require an enclosing module selected by a cover-module annotation or
// an exact name from autocounter-covermodules.txt. Resolve selected records to
// SSA only after filtering. Preserves all annotations; counter synthesis owns
// their eventual consumption. On failure, leaves events and IR unchanged.
mlir::LogicalResult analyzeSelectedAutoCounterEvents(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    llvm::SmallVectorImpl<AutoCounterEvent> &events, std::string &error);

// Match AutoCounterTransform's cleanup when EnableAutoCounter is false:
// the annotations must no longer hold their target values alive.
mlir::LogicalResult dropDisabledAutoCounterAnnotations(
    circt::firrtl::CircuitOp circuit, unsigned &removed,
    std::string &error);
} // namespace goldengate
