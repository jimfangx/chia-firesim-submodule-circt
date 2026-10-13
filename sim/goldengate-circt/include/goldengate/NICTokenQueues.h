// See LICENSE for license details.
#ifndef GOLDENGATE_NIC_TOKEN_QUEUES_H
#define GOLDENGATE_NIC_TOKEN_QUEUES_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Bind SimpleNIC's fifteen pipe channels and two depth-ten token queues.
// CPU stream packet adapters and MMIO controls remain explicit boundaries.
// A circuit with no SimpleNIC BridgeIO annotation is left unchanged.
mlir::LogicalResult addNICTokenQueues(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
}
#endif
