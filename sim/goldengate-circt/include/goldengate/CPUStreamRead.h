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
// Resolve all numbered Rocket outgoing queues in constructor order through
// active wrapper connects. Each payload/count pair must reach its own live
// 6144x512 TracerV queue, with matching bank identity and storage. This
// read-only preflight preserves both output lists on failure; Print streams
// are appended in their host constructor order by mapPrintBridgeCPUStreams.
mlir::LogicalResult deriveRocketCPUStreamPorts(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<CPUStreamSourcePort> &sources,
    llvm::SmallVectorImpl<CPUStreamCountPort> &counts, std::string &error);
// Resolve the supported NIC + TracerV catalog from live queue connections.
// Sink descriptors use the same buffer geometry and a separate ordered list.
// Sources precede sinks in the count bank. Directional DMA indices are assigned
// independently; NIC precedes TracerV in SFC bridge constructor order. All
// output lists and IR remain unchanged on rejection.
mlir::LogicalResult deriveCPUStreamPorts(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<CPUStreamSourcePort> &sources,
    llvm::SmallVectorImpl<CPUStreamSourcePort> &sinks,
    llvm::SmallVectorImpl<CPUStreamCountPort> &counts, std::string &error);
// Consume Decoupled512 outputs into one AXI AR/R interface. Allocate equal
// windows using ceilLog2(64 * max(depth)); every stream counter follows the
// global R handshake, as in CPUManagedStreamEngine. No annotations consumed.
// Validate all ports, identities and address allocations before mutation.
mlir::LogicalResult addCPUStreamRead(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamSourcePort> sources, std::string &error);
// F2's BAR4 backend reads one 512-bit stream beat with non-destructive PCI
// loads (ID0x20, LEN0; naturally aligned ARSIZE2..5 or shell-normalized6).
// Reads retain the head regardless of PCI read speculation/coalescing. A write
// of one to the matching source-count MMIO word consumes exactly one beat.
// Full-width DMA bursts retain SFC behavior; speculative PCI loads also keep
// the head. This is an F2 integration extension, not an SFC proof.
// Apply once after stream composition, only on the F2 platform.
mlir::LogicalResult enableF2CPUStreamWordReads(
    circt::firrtl::CircuitOp circuit, std::string &error);
// Read-only, strict structural validation of the complete SDK profile; header
// generation must not trust a metadata flag without its live handshake logic.
mlir::LogicalResult validateF2CPUStreamAckRoute(
    circt::firrtl::CircuitOp circuit, std::string &error);
mlir::LogicalResult validateF2CPUStreamWordReads(
    circt::firrtl::FModuleOp engine, std::string &error);
}
#endif
