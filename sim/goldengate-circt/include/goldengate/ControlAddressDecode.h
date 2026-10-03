// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLADDRESSDECODE_H
#define GOLDENGATE_CONTROLADDRESSDECODE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
namespace goldengate {
struct ControlMMIORegion {
  llvm::StringRef name;
  uint64_t start, size;
};
// Port NastiRecursiveInterconnect.routeSel and NastiRouter.routeEncode.
// Region order defines slave indices. The all-zero one-hot selection encodes
// the error slave at index regions.size(). Input: uninstantiated
// GGControlErrorWrapper with retained annotations. Add an exposed ctrl_decode_*
// boundary; request queues, bank connections and response routing follow later.
// All validation precedes mutation, including bounds, overlap and name checks.
mlir::LogicalResult addControlAddressDecode(circt::firrtl::CircuitOp circuit,
    unsigned addressBits, llvm::ArrayRef<ControlMMIORegion> regions,
    std::string &error);
}
#endif
