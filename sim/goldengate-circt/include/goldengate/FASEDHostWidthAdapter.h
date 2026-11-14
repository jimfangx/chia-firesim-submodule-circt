// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Preserve AXI transaction IDs/order while splitting target beats onto the
// 64-bit host port. Read descriptors and partial words are tracked per ID.
mlir::LogicalResult
materializeFASEDHostWidthAdapter(circt::firrtl::CircuitOp circuit,
                                 circt::firrtl::BundleType target,
                                 circt::firrtl::BundleType host,
                                 circt::firrtl::FModuleOp &result,
                                 std::string &error);
} // namespace goldengate
