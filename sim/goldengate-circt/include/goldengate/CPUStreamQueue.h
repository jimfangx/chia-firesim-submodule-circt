// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
namespace goldengate {
// Caller validates the boundary and reserves a unique module name before use.
// Creates an internal 6144x512 FIFO with clock/reset, enq/deq Decoupled ports
// and UInt<13> occupancy. Stream identity/index allocation belongs to callers.
circt::firrtl::FModuleOp createCPUStreamQueue6144(
    circt::firrtl::CircuitOp circuit, llvm::StringRef name);
}
