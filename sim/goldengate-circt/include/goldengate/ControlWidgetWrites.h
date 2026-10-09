// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWIDGETWRITES_H
#define GOLDENGATE_CONTROLWIDGETWRITES_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>
namespace goldengate {
struct ControlWidgetPort { std::string widget; std::string port; };
// Explicit implemented-bank catalog, in registration order. Resolve slaves by
// allocated identity; require unique nonempty widget and port names.
mlir::LogicalResult bindControlWidgetWrites(circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<ControlWidgetPort> widgets, std::string &error);
// Bind the seven implemented U250 MCRFile slaves to NastiRouter's AW/W
// dispatcher using widget identities in the decoded allocation. The remaining
// control bundles contain only AR/B/R. Additional
// AW/W metadata is broadcast from shared master ports, as in NastiRouter.
// Unimplemented slaves and response arbitration remain explicit boundaries.
mlir::LogicalResult bindControlWidgetWrites(circt::firrtl::CircuitOp circuit,
                                           std::string &error);
}
#endif
