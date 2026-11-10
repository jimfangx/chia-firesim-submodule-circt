// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// FPGATopImp.genHeader's GET_METASIM_INTERFACE_CONFIG section and genVHeader.
// Analyze aggregate FPGATop/F1Shim ports before adding .const.h and .const.vh
// output annotations. The supported U250 and F2 shells have one memory channel,
// CPU-managed AXI4, and no target QSFP/FPGA-managed endpoints. F2 exposes
// an inactive physical PCIM port and no physical QSFP ports. Bridge
// constructors are emitted by separate header preparation functions.
mlir::LogicalResult prepareMetasimInterfaceHeader(
    circt::firrtl::CircuitOp circuit, llvm::StringRef targetName,
    std::string &error, bool isF2 = false);
} // namespace goldengate
