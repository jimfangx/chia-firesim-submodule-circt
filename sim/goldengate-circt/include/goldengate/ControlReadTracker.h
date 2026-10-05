// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLREADTRACKER_H
#define GOLDENGATE_CONTROLREADTRACKER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// NastiRouter's 64-slot, 12-bit-ID ReorderQueue. Requires an uninstantiated
// AR dispatch top, retained raw annotations/read bindings and the decoder's
// ordered one-to-63-region catalog. Count determines route width and the error
// dequeue index. Accepted last R beats retire mapped/error slots; unbound
// decoded-slave completion inputs stay explicit. Reject before mutation;
// retain annotation classes/payloads and transfer only copied port targets.
mlir::LogicalResult addControlReadTracker(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
}
#endif
