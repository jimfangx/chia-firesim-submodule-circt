// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_MMIO_H
#define GOLDENGATE_FASED_FRFCFS_MMIO_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <string>
namespace goldengate {
// Exact selected FRFCFS16GBQuadRankLLC4MB MCR order, independent of maxFlight.
// Each row is a readable 32-bit host word at four times its array index.
// RW words retain all 32 bits; width describes the narrower model boundary.
// Empty field and index -1 select a scalar port; other rows select a field
// and/or vector element. RO status ports are inputs, RW configuration ports
// outputs.
struct FASEDFRFCFSMMIORegister {
  std::string name, port, field;
  int index;
  unsigned width;
  uint32_t initial;
  bool writeable;
};
llvm::ArrayRef<FASEDFRFCFSMMIORegister> getFASEDFRFCFSMMIORegisters();
// Omit LLC and first-ready settings for profiles which do not instantiate them.
llvm::ArrayRef<FASEDFRFCFSMMIORegister>
getFASEDFRFCFSMMIORegisters(circt::firrtl::CircuitOp circuit);
// Host-clock configuration/error bank. Model statistics remain external and
// retain the targetFire/targetReset schedule of their owning timing model.
// Free-symbol preflight is atomic and leaves result unchanged on rejection.
mlir::LogicalResult materializeFASEDFRFCFSMMIO(circt::firrtl::CircuitOp circuit,
                                               circt::firrtl::FModuleOp &result,
                                               std::string &error);
} // namespace goldengate
#endif
