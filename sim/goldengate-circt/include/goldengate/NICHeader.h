// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// SimpleNICBridgeModule.genHeader for normal NICs or the native compile-time
// loopback driver selected from validated live hardware mode. Requires
// the validated CPU stream header. Derives MMIO addresses and stream indices
// from live FIRRTL allocations; changes only the sole .const.h OutputFile.
mlir::LogicalResult prepareNICHeader(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
// Compile-time LoopbackNIC is free-running: its CPU adapters are disabled and
// its sole done register needs no host service. Validate that live contract and
// emit the dedicated native driver constructor, preserving IR/annotations.
// This explicitly replaces SFC's incompatible normal-mode driver constructor;
// it does not change normal NIC or runtime +nic-loopbackN behavior.
mlir::LogicalResult emitNICLoopbackHeader(circt::firrtl::CircuitOp circuit,
                                        std::string &header,
                                        std::string &error);
}
