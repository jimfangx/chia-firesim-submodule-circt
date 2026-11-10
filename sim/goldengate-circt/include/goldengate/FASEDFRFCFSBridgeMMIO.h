// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_BRIDGE_MMIO_H
#define GOLDENGATE_FASED_FRFCFS_BRIDGE_MMIO_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Attach the independently materialized selected 68-word FRFCFS bank to the
// active FRFCFS bridge. Close model programming/statistics and ingress-relaxed
// ports; observe the existing functional host responses without intercepting
// their ready/valid path. Host reset controls bank state; target reset/fire
// continue to control the timing model only. Expose fasedBridge_mcr for the
// common MCRFile transport. Retain diagnostic model outputs and other ports.
// Requires unique uninstantiated top/bank and exact typed boundaries. All
// validation precedes mutation; rejection preserves IR and annotations.
mlir::LogicalResult attachFASEDFRFCFSMMIO(circt::firrtl::CircuitOp circuit,
                                          circt::firrtl::FModuleOp bank,
                                          std::string &error);
} // namespace goldengate
#endif
