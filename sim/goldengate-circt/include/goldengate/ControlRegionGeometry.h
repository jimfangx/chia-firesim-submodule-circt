// See LICENSE for license details.
#pragma once
#include "llvm/Support/MathExtras.h"
#include <cstdint>
#include <limits>

namespace goldengate {
// Region count is independent of NastiRouter's 64 outstanding transaction
// slots. One-hot routes have one bit per normal slave; encoded routes also
// represent the error slave at index count. Keep widths and nonnegative i32
// region indices representable, including that extra endpoint.
inline bool validControlRegionCount(uint64_t count) {
  return count && count < uint64_t(std::numeric_limits<int32_t>::max());
}
inline unsigned controlRouteWidth(unsigned count) {
  return llvm::Log2_64_Ceil(uint64_t(count) + 1);
}
} // namespace goldengate
