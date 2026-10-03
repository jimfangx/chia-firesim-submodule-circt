// See LICENSE for license details.
#ifndef GOLDENGATE_LOADMEMWRITER_H
#define GOLDENGATE_LOADMEMWRITER_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Map U250 LoadMemWriter's addr34/data64/maxBurst32 request and burst FSM.
mlir::LogicalResult addLoadMemWriter(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
// Queue writes and arbitrate them ahead of the recorded U250 zero-fill request.
mlir::LogicalResult addLoadMemRequests(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
// Decode write address/length/zero words 0,1,2,3 and ZERO_FINISHED word 5.
mlir::LogicalResult addLoadMemWriteMMIO(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
// Pack W_DATA decoded UInt32 writes into 32 buffered UInt64 writer beats.
mlir::LogicalResult addLoadMemWriteData(circt::firrtl::CircuitOp circuit,
                                     std::string &error);
// Queue decoded R_ADDRESS_L writes and preserve R_ADDRESS_H readback.
mlir::LogicalResult addLoadMemReadRequests(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
// Split one buffered host UInt64 R beat into two low-word-first R_DATA reads.
mlir::LogicalResult addLoadMemReadData(circt::firrtl::CircuitOp circuit,
                                    std::string &error);
}
#endif
