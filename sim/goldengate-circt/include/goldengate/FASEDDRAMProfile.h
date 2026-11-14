// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_DRAM_PROFILE_H
#define GOLDENGATE_FASED_DRAM_PROFILE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
namespace goldengate {
// Absent selection preserves the original FRFCFS/LLC component API. Production
// sets this separately from capacity: FCFS and LatencyPipe both have base16.
inline bool isFASEDFCFS(circt::firrtl::CircuitOp circuit) {
  auto profile =
      circuit->getAttrOfType<mlir::StringAttr>("goldengate.fasedDRAMProfile");
  return profile && profile.getValue() == "fcfs";
}
inline bool hasFASEDLLC(circt::firrtl::CircuitOp circuit) {
  auto profile =
      circuit->getAttrOfType<mlir::StringAttr>("goldengate.fasedDRAMProfile");
  return !profile || profile.getValue() == "frfcfs-llc";
}
inline unsigned getFASEDDRAMBaseCapacity(circt::firrtl::CircuitOp circuit) {
  return isFASEDFCFS(circuit) ? 16 : 32;
}
} // namespace goldengate
#endif
