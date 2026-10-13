// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Buffer the normal NIC packet boundary in both directions, with independent
// 3072x512 CPUManagedStreamEngine queues and UInt<12> occupancy outputs.
// Stream names/directions are retained, but AXI indices/MMIO are not allocated.
// Requires one constructor-free NIC and an uninstantiated packet-adapter top.
// No NIC is a no-op. Rebuild hierarchy analyses after success.
mlir::LogicalResult addNICCPUStreamQueues(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
