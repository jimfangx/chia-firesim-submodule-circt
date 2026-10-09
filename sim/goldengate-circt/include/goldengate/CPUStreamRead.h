// See LICENSE for license details.
#ifndef GOLDENGATE_CPUSTREAMREAD_H
#define GOLDENGATE_CPUSTREAMREAD_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "goldengate/CPUStreamCountBank.h"
#include <string>
namespace goldengate {
// Explicit allocator order of buffered outgoing streams on the active top.
// The supported U250 boundary has 512-bit payloads and 64-bit DMA addresses.
struct CPUStreamSourcePort {
  std::string streamName;
  std::string portName;
  unsigned depth;
};
// Resolve the recorded Rocket outgoing queue through active wrapper connects.
// Payload and count must reach the same 6144x512 TracerV queue instance. This
// read-only preflight preserves both output lists on failure; Print streams
// are appended in their host constructor order by mapPrintBridgeCPUStreams.
mlir::LogicalResult deriveRocketCPUStreamPorts(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<CPUStreamSourcePort> &sources,
    llvm::SmallVectorImpl<CPUStreamCountPort> &counts, std::string &error);
// Consume Decoupled512 outputs into one AXI AR/R interface. Allocate equal
// windows using ceilLog2(64 * max(depth)); every stream counter follows the
// global R handshake, as in CPUManagedStreamEngine. No annotations consumed.
// Validate all ports, identities and address allocations before mutation.
mlir::LogicalResult addCPUStreamRead(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamSourcePort> sources, std::string &error);
}
#endif
