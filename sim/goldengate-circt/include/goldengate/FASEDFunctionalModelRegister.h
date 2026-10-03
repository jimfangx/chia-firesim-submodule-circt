// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FUNCTIONAL_MODEL_REGISTER_H
#define GOLDENGATE_FASED_FUNCTIONAL_MODEL_REGISTER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Decoded lane zero is relaxFunctionalModel at byte offset 72.
mlir::LogicalResult addFASEDFunctionalModelRegister(circt::firrtl::CircuitOp circuit,
                                                 std::string &error);
}
#endif
