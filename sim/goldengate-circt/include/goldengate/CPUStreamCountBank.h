// See LICENSE for license details.
#ifndef GOLDENGATE_CPUSTREAMCOUNTBANK_H
#define GOLDENGATE_CPUSTREAMCOUNTBANK_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include <string>
namespace goldengate {
// Ordered queue occupancy outputs of the active simulator top. Stream ordering
// is supplied by the stream allocator: CPUManagedStreamEngine elaborates source
// counts before sink counts (its constructor serializes sink descriptors first).
// Counts remain live during read backpressure.
struct CPUStreamCountPort {
  std::string streamName;
  std::string portName;
  unsigned countBits;
};
// Consume these outputs into read-only 32-bit MCR words at byte offsets 4*i.
// Validate the complete binding before adding modules or retargeting annotations.
mlir::LogicalResult addCPUStreamCountBank(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<CPUStreamCountPort> counts, std::string &error);
}
#endif
