// See LICENSE for license details.
#ifndef GOLDENGATE_NIC_MMIO_BANK_H
#define GOLDENGATE_NIC_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Attach the normal-mode SimpleNICBridge six-word decoded register bank to
// each active duplex CPU widget. Multiple widgets use distinct indexed control
// banks/MCR ports; the single-widget ABI is retained. Preflight all widgets
// before mutation. Global Nasti binding/allocation is separate.
mlir::LogicalResult addNICMMIOBank(circt::firrtl::CircuitOp circuit,
                                  std::string &error);
// Attach the compile-time LoopbackNIC one-word done bank at the loopback
// token boundary. This matches Widget.genROReg: unreset sample, writable by
// MCR with write priority, ignores strobes. It does not allocate CPU streams.
mlir::LogicalResult addNICLoopbackMMIOBank(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
// Bind the validated MMIO registry/bank to the shared native Lib.scala
// MCRFile transport. Global widget allocation and platform binding are separate.
mlir::LogicalResult mapNICBridgeControl(circt::firrtl::CircuitOp circuit,
                                      unsigned addressBits, unsigned idBits,
                                      std::string &error);
}
#endif
