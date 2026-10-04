// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace goldengate {
/// Disabled PrintSynthesis, matching Scala PrintSynthesis.execute.
/// Required input invariants: retained rawAnnotations and SynthPrints=false.
/// Annotations consumed: all SynthPrintfAnnotation records, including duplicates.
/// Annotations produced: none.
/// IR mutations: rawAnnotations only; printf operations remain unchanged.
/// Analyses required/preserved: none/all operation and hierarchy analyses.
/// Output invariants: other annotation identities and order are preserved;
/// disabled selections need not resolve to existing operations.
mlir::LogicalResult dropDisabledPrintAnnotations(
    circt::firrtl::CircuitOp circuit, unsigned &removed, std::string &error);

struct PrintStub {
  circt::firrtl::PrintFOp print;
  circt::firrtl::WireOp bundle;
  mlir::Value clock;
  std::string target, clockTarget, formatString;
};
// PrintSynthesis step 1: copy enable and typed arguments into a passive bundle
// and append BridgeTopWiringAnnotation. Keep printf operations and their input
// annotations until hierarchy wiring/bridge construction completes. Return
// decoded format strings alongside operation identities for that next step.
// Requires LowerTypes and ExpandWhens. Invalid selections fail atomically.
mlir::LogicalResult synthesizePrintStubs(
    circt::firrtl::CircuitOp circuit, llvm::SmallVectorImpl<PrintStub> &stubs,
    std::string &error);
} // namespace goldengate
