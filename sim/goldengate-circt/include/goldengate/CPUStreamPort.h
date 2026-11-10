// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult assembleCPUStreamPort(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
/// Select the original memory platform, selected/expanded Print B boundary or
/// explicit NIC loopback B boundary. The NIC-only boundary derives its paired
/// ordered 3072x512 sink/source catalog. The full platform validates its live
/// directional catalogs independently, permitting multiple NIC sinks and mixed
/// NIC/TracerV/Print sources with independently derived address windows.
/// Historical scalar-only profiles retain their original geometry checks.
/// Validate its exact scalar forwarding before generating the shared AXI adapter.
mlir::LogicalResult attachCPUStreamPort(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
}
