// See LICENSE for license details.
#ifndef GOLDENGATE_TSI_TOKEN_ENGINE_H
#define GOLDENGATE_TSI_TOKEN_ENGINE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// TSIBridge HostPort scheduler; word queues and MMIO remain explicit boundaries.
mlir::LogicalResult addTSITokenEngine(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
}
#endif
