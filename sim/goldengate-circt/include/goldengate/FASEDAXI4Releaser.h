// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_AXI4_RELEASER_H
#define GOLDENGATE_FASED_AXI4_RELEASER_H

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
// Selected 35/64/4-bit AXI4 response release semantics from Util.scala.
// Two one-entry pipe queues retain occupancy only; metadata and egress payloads
// are not stored. State/reset advance only on targetFire. Egress requests and
// payload/ready forwarding remain combinational, including invalid cycles.
// A symbol collision leaves both the circuit and result unchanged.
mlir::LogicalResult
materializeFASEDAXI4Releaser(circt::firrtl::CircuitOp circuit,
                             circt::firrtl::FModuleOp &result,
                             std::string &error);
} // namespace goldengate
#endif
