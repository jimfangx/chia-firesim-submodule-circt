// See LICENSE for license details.
#ifndef GOLDENGATE_CPUMANAGEDSTREAMHEADER_H
#define GOLDENGATE_CPUMANAGEDSTREAMHEADER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
enum class CPUStreamHeaderBoundary { Complete, SelectedOutgoing };
// Complete validates the empty incoming AXI engine as well as outgoing queues.
// SelectedOutgoing validates the completed selected control master, whose CPU
// write transport has not yet been assembled; it must have no incoming engine
// or write ports. This emits allocation collateral for that partial boundary.
mlir::LogicalResult prepareCPUManagedStreamHeader(
    circt::firrtl::CircuitOp circuit, std::string &error,
    CPUStreamHeaderBoundary boundary = CPUStreamHeaderBoundary::Complete);
}
#endif
