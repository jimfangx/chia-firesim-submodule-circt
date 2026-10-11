// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Compose native Rocket constructors into the existing interface .const.h.
// Validate all live allocated banks and CPU transport on a clone; publish
// only rawAnnotations after every bridge check succeeds. The original
// interface header and complete annotation archive survive any rejection.
mlir::LogicalResult prepareRocketDriverHeaders(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
