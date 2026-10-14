// See LICENSE for license details.
#ifndef GOLDENGATE_NIC_MMIO_BANK_H
#define GOLDENGATE_NIC_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Attach the normal-mode SimpleNICBridge six-word decoded register bank to
// the active duplex CPU transport. Global Nasti binding/allocation is separate.
mlir::LogicalResult addNICMMIOBank(circt::firrtl::CircuitOp circuit,
                                  std::string &error);
}
#endif
