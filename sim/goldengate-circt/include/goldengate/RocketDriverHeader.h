// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Compose native, NIC or expanded Print Rocket constructors into the existing interface .const.h.
// Validate all live allocated banks and CPU transport on a clone; publish
// only rawAnnotations after every bridge check succeeds. The original
// interface header and complete annotation archive survive any rejection.
// Expanded Print includes its allocated factories, embedded decoders and CPU
// descriptors in the same transaction; no separate decoder file is required.
// A retained SimpleNIC bridge additionally validates and emits its allocated
// constructor after the shared incoming/outgoing CPU stream descriptors.
mlir::LogicalResult prepareRocketDriverHeaders(
    circt::firrtl::CircuitOp circuit, std::string &error);
}
