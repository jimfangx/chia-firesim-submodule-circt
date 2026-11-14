// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_FRFCFS_BRIDGE_H
#define GOLDENGATE_FASED_FRFCFS_BRIDGE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Attach the selected DDR3 timing model (FIFO or first-ready, optional LLC) to
// the active WriteEgress wrapper. Requires explicit base16 (FCFS) or base32
// (FRFCFS), 35-bit addresses and four-bit IDs. Data width, transfer lengths,
// per-ID reuse and flight limit come from the constructor. Closes read/write
// egress ports. Other ports and annotation identities are retained. Every
// remaining model port is exported as fased_frfcfs_<modelPortName>, with its
// exact type/direction (including observation aggregates). Programming inputs
// are wayBits:u2, setBits:u4, blockBits:u3, activeMSHRs:u4, timings:the native
// DRAM timing bundle, schedulerWindowSize:u3, openPagePolicy:u1,
// backendLatency:u12, bank/rank/rowAddrOffset:u32, bankAddrMask:u3,
// rankAddrMask:u2, rowAddrMask:u26. Types are copied, never approximated.
// This boundary deliberately leaves MMIO, global reset, functional host memory,
// histogram/host statistics and platform/AutoILA attachment to later stages.
// Failure leaves the complete circuit unchanged, including attributes/body.
mlir::LogicalResult addFASEDFRFCFSBridge(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
} // namespace goldengate
#endif
