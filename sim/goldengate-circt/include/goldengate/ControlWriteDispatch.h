// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLWRITEDISPATCH_H
#define GOLDENGATE_CONTROLWRITEDISPATCH_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// U250 NastiRouter AW/W selection, readiness priority and surviving payloads.
// Consumes selected-ready boundaries and the error endpoint's AW/W inputs;
// connects the live decoder and queued W routes and the existing error slave.
// Eleven widget slave boundaries remain explicit until their control bundles
// are bound to the interconnect. Payload widths: AW addr25/len8/id12, W data32/last1.
mlir::LogicalResult addControlWriteDispatch(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
}
#endif
