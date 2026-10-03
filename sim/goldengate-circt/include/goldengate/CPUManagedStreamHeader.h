// See LICENSE for license details.
#ifndef GOLDENGATE_CPUMANAGEDSTREAMHEADER_H
#define GOLDENGATE_CPUMANAGEDSTREAMHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult prepareCPUManagedStreamHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
