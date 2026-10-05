// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace goldengate {
// Materialize PrintBridge.scala's combinational PrintRecordBag payload from
// retained BridgeIO constructors. Constructor order controls both packed bits
// and goldengate.printPayload offsets. Queues/ROI/stream control remain pending;
// annotations are retained. Validate all constructors before modifying IR.
mlir::LogicalResult materializePrintBridgePayloads(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &modules,
    std::string &error);
} // namespace goldengate
