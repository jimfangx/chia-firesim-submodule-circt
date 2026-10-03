// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Add the FAME host clock/reset inputs to a model and its instances. The
// existing parent host inputs are connected to each new instance port. Create
// the targetCycleFinishing wire for subsequent FAME channel rewrites.
// Annotations on existing model and instance ports are preserved by CIRCT's
// port insertion APIs; no annotation is consumed or produced here.
mlir::LogicalResult addFAMEHostControl(circt::firrtl::CircuitOp circuit,
                                      circt::firrtl::FModuleOp model,
                                      std::string &error);
} // namespace goldengate
