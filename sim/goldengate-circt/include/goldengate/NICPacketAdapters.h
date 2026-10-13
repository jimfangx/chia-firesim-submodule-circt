// See LICENSE for license details.
#ifndef GOLDENGATE_NIC_PACKET_ADAPTERS_H
#define GOLDENGATE_NIC_PACKET_ADAPTERS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Normal SimpleNIC mode: seven 66-bit queue tokens per 512-bit CPU packet.
// Requires the queue boundary; stream allocation and MMIO remain later work.
// No NIC annotation leaves the circuit unchanged. Rebuild hierarchy analyses.
mlir::LogicalResult addNICPacketAdapters(circt::firrtl::CircuitOp circuit,
                                       std::string &error);
}
#endif
