// See LICENSE for license details.
#ifndef GOLDENGATE_FASEDHEADER_H
#define GOLDENGATE_FASEDHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
mlir::LogicalResult prepareFASEDHeader(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
#endif
