// See LICENSE for license details.
#ifndef GOLDENGATE_CONTROLADDRESSDECODE_H
#define GOLDENGATE_CONTROLADDRESSDECODE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>
namespace goldengate {
struct ControlMMIORegion {
  llvm::StringRef name;
  uint64_t start, size;
};
struct ControlMMIOWidget {
  llvm::StringRef name;
  uint64_t registerCount;
  std::optional<uint64_t> customSize = std::nullopt;
};
// Read an already materialized widget's register registry from the named
// FIRRTL modules (possibly sparse fragments, as for LoadMem). Requires unique
// names, contiguous four-byte offsets and permissions, and an MCRFile whose
// read/write vectors have exactly the registry's UInt32 token count. No
// annotations are consumed/produced, no IR or analyses are mutated. The result
// borrows widgetName; leave it unchanged on failure. Registration order is
// supplied by the caller, independently of module or metadata row order.
mlir::LogicalResult deriveControlMMIOWidget(circt::firrtl::CircuitOp circuit,
    llvm::StringRef widgetName, llvm::StringRef mcrModule,
    llvm::ArrayRef<llvm::StringRef> registerModules, ControlMMIOWidget &widget,
    std::string &error);
// Widget.memRegionSize / HasWidgets.addrMap: round the 32-bit control bank's
// byte size up to a power of two (or use customSize), stable-sort by decreasing
// size, then assign contiguous regions from zero. Input order is widget
// registration order and determines slave order for equal-sized banks. Names
// borrow the descriptors' storage. On failure, leave regions unchanged.
mlir::LogicalResult allocateControlMMIORegions(unsigned addressBits,
    llvm::ArrayRef<ControlMMIOWidget> widgets,
    llvm::SmallVectorImpl<ControlMMIORegion> &regions, std::string &error);
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
