// See LICENSE for license details.
#ifndef GOLDENGATE_CPUSTREAMREAD_H
#define GOLDENGATE_CPUSTREAMREAD_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>
namespace goldengate {
// Explicit allocator order of buffered outgoing streams on the active top.
// The supported U250 boundary has 512-bit payloads and 64-bit DMA addresses.
struct CPUStreamSourcePort {
  std::string streamName;
  std::string portName;
  unsigned depth;
};
// Consume Decoupled512 outputs into one AXI AR/R interface. Allocate equal
// windows using ceilLog2(64 * max(depth)); every stream counter follows the
// global R handshake, as in CPUManagedStreamEngine. No annotations consumed.
// Validate all ports, identities and address allocations before mutation.
mlir::LogicalResult addCPUStreamRead(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamSourcePort> sources, std::string &error);
}
#endif
