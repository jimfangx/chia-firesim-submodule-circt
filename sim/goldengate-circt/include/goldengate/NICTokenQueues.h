// See LICENSE for license details.
#ifndef GOLDENGATE_NIC_TOKEN_QUEUES_H
#define GOLDENGATE_NIC_TOKEN_QUEUES_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind each SimpleNIC's fifteen pipe channels and two depth-ten token queues.
// Multiple widgets use distinct indexed boundary names and independent gates;
// a single widget retains its established unindexed ABI.
// CPU stream packet adapters and MMIO controls remain explicit boundaries.
// A circuit with no SimpleNIC BridgeIO annotation is left unchanged.
mlir::LogicalResult addNICTokenQueues(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
// Compile-time LoopbackNIC's channel binding, two depth-ten queues and actual
// ten-token seed/forward scheduler. This explicit boundary preserves keep bits
// and exposes only done; the normal CPU adapters/MMIO ABI cannot consume it.
// Full loopback platform assembly and driver integration remain separate.
mlir::LogicalResult addNICLoopbackTokenQueues(circt::firrtl::CircuitOp circuit,
                                            std::string &error);
}
#endif
