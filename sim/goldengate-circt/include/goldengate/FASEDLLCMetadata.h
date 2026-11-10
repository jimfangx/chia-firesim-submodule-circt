// See LICENSE for license details.
#ifndef GOLDENGATE_FASED_LLC_METADATA_H
#define GOLDENGATE_FASED_LLC_METADATA_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Selected LLC metadata organization: 4096 sets, eight ways, 32-bit stored tags.
// lookupTag/stage1Tag remain 35 bits: comparison zero extends stored tags, while
// writes truncate. An asynchronous memory read plus explicit unreset enabled
// output registers implements read latency one and holds metadata on disabled
// cycles. Writes have latency one; RUW is undefined, with old data captured by
// the read registers. No reset/initial contents; programming-time zeroing is a
// platform duty.
// targetFire gates memory enables, stage1 address registers and the replacement
// LFSR. Only the LFSR has synchronous reset (seed one in exposed bit order).
// Ports: clock/reset/targetFire, lookupStart, lookupSet:12, lookupTag:35,
// wayBits:2, stage1Valid, stage1IsWrite, stage1Set:12, stage1Tag:35,
// hitValid, emptyValid, evictDirty, evictTag:32, selectedWayOH:8,
// metadata:vector8<{tag:32,valid:1,dirty:1}>, lfsr:16.
// Helper insertion fails atomically if its symbol already exists.
mlir::LogicalResult materializeFASEDLLCMetadata(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp &result,
    std::string &error);
}
#endif
