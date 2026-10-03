// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Port UARTBridgeModule's token-driven serial engine.
mlir::LogicalResult addUARTSerialEngine(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
// Add the oracle's two Queue(UInt(8.W), 128) byte buffers. The outer byte
// interfaces are the queue heads/tails used by subsequent UART MMIO mapping.
mlir::LogicalResult addUARTByteQueues(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
// Replace the queue byte ports with UARTBridgeModule's six decoded MCR words.
mlir::LogicalResult addUARTMMIOBank(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
}
