// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// EC2F2Config adds WithPCIMPorts; midasexamples.DefaultF2Config uses F2Config
// directly and has no physical PCIM interface.
inline bool hasF2PCIM(circt::firrtl::CircuitOp circuit) {
  auto selected = circuit->getAttrOfType<mlir::BoolAttr>("goldengate.f2PCIM");
  return !selected || selected.getValue();
}
mlir::LogicalResult assembleF1Shim(circt::firrtl::CircuitOp circuit, std::string &error,
                                   bool isF2 = false);
}
