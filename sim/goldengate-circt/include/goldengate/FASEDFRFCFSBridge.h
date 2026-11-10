// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_BRIDGE_H
#define GOLDENGATE_FASED_FRFCFS_BRIDGE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Attach the independently composed FRFCFS/LLC timing model to the active
// WriteEgress wrapper. Requires explicit base32 and selected constructor
// 35/64/4, transfer8, (maxFlight,idReuse)=(10,1)/(20,2). Closes timing and
// functional read/write egress ports. Other ports and annotation identities
// are retained.
// Every remaining model port is exported as fased_frfcfs_<modelPortName>, with
// its exact type/direction (including observation aggregates). Programming
// inputs are wayBits:u2, setBits:u4, blockBits:u3, activeMSHRs:u4, timings:the
// native DRAM timing bundle, schedulerWindowSize:u3, openPagePolicy:u1,
// backendLatency:u12, bank/rank/rowAddrOffset:u32, bankAddrMask:u3,
// rankAddrMask:u2, rowAddrMask:u26. Types are copied, never approximated.
// This boundary deliberately leaves MMIO, global reset, functional host memory,
// histogram/host statistics and platform/AutoILA attachment to later stages.
// Failure leaves the complete circuit unchanged, including attributes/body.
mlir::LogicalResult addFASEDFRFCFSBridge(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
} // namespace goldengate
#endif
