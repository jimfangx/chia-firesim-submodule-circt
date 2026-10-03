// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_RESPONSE_ERRORS_H
#define GOLDENGATE_FASED_RESPONSE_ERRORS_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Decoded lanes 0/1 are read-only rrespError/brespError at bytes 76/80.
mlir::LogicalResult addFASEDResponseErrors(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
}
#endif
