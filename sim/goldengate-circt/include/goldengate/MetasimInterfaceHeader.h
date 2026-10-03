// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// FPGATopImp.genHeader's GET_METASIM_INTERFACE_CONFIG section. Analyze the
// actual aggregate FPGATop/F1Shim ports before adding the .const.h output
// annotation. The current U250 shell has one memory channel, CPU-managed
// AXI4, and no target QSFP/FPGA-managed endpoints. Bridge constructors remain
// a separate, unfinished part of the driver header.
mlir::LogicalResult prepareMetasimInterfaceHeader(
    circt::firrtl::CircuitOp circuit, llvm::StringRef targetName,
    std::string &error);
} // namespace goldengate
