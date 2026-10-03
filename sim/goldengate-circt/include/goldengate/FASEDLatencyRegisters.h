// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_LATENCY_REGISTERS_H
#define GOLDENGATE_FASED_LATENCY_REGISTERS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Decode lanes 0/1 are the write/read latency words at byte offsets 0/4.
mlir::LogicalResult addFASEDLatencyRegisters(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
