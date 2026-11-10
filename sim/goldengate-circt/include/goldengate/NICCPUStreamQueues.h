// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Buffer the normal NIC packet boundary in both directions, with independent
// 3072x512 CPUManagedStreamEngine queues and UInt<12> occupancy outputs.
// Stream names/directions are retained, but AXI indices/MMIO are not allocated.
// Requires constructor-free NICs and an uninstantiated packet-adapter top.
// Multiple widgets use validated packet bindings and independent indexed queues;
// one widget preserves its existing ABI. Stream indices remain a later stage.
// No NIC is a no-op. Rebuild hierarchy analyses after success.
mlir::LogicalResult addNICCPUStreamQueues(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
// Preserve compile-time loopback's allocated CPU streams: independent real
// 3072x512 queues, incoming dequeue disabled and outgoing enqueue disabled.
// Requires the active loopback token boundary and its ordered mode bindings.
// Host reset is required before traffic, as for the SFC adapter selectors.
// Exposes streams/counts for the shared CPU catalog; does not create a driver.
mlir::LogicalResult addNICLoopbackCPUStreamQueues(circt::firrtl::CircuitOp circuit,
                                               std::string &error);
}
