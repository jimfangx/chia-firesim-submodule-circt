// See LICENSE for license details.
#ifndef GOLDENGATE_TSI_MMIO_BANK_H
#define GOLDENGATE_TSI_MMIO_BANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// TSI's nine sampled/data/pulse registers at the decoded MCRFile boundary.
mlir::LogicalResult addTSIMMIOBank(circt::firrtl::CircuitOp circuit,
                                  std::string &error);
}
#endif
