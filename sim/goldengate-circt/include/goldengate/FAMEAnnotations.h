// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// FAMETransform drops DontTouchAnnotation whose ReferenceTarget.moduleTarget
// is a transformed model. Consume those retained targets and attached model
// port/body annotations before rewriting ports. Preserve other classes,
// wrapper/non-model targets, and inner symbols. Preflight before mutation;
// does not invalidate hierarchy, channel or target identity analyses.
mlir::LogicalResult consumeFAMEModelDontTouches(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> models, std::string &error);

// Transfer retained local wrapper DontTouch targets after a channel rewrite.
// The old ground port is gone; the replacement must resolve to a payload
// field on the live CIRCT wrapper. Other classes and hierarchical/internal
// targets remain unchanged. Attached port metadata is handled separately by
// the channel rewrite's annotation preflight.
mlir::LogicalResult transferFAMEWrapperDontTouch(
    circt::firrtl::CircuitOp circuit, llvm::StringRef oldTarget,
    llvm::StringRef payloadTarget, std::string &error);
} // namespace goldengate
