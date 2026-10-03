// See LICENSE for license details.
#ifndef GOLDENGATE_TRACERVTOKENENGINE_H
#define GOLDENGATE_TRACERVTOKENENGINE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Map Rocket's one-retirement TracerV HostPort to native token/stream FIRRTL.
// Trigger selection, initDone/traceEnable MMIO and stream storage remain explicit
// control and stream boundaries until their subsequent compiler stages.
mlir::LogicalResult addTracerVTokenEngine(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Connect the engine to Rocket's decoded 15-word configuration bank, preserving
// host-cycle PC/instruction state and the delayed cycle/PC limit registers.
mlir::LogicalResult addTracerVTriggerConfig(circt::firrtl::CircuitOp circuit,
                                           std::string &error);
// Buffer the sole Rocket CPU stream with its 6144-entry synchronous-read queue.
mlir::LogicalResult addTracerVStreamQueue(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Drain the buffered Rocket stream through the U250 CPU AXI read interface.
mlir::LogicalResult addCPUStreamRead(circt::firrtl::CircuitOp circuit,
                                   std::string &error);
// Bind the live UInt<13> queue count to the sole read-only CPU stream MCR word.
mlir::LogicalResult addCPUStreamCountBank(circt::firrtl::CircuitOp circuit,
                                        std::string &error);
// Preserve the AW/W/B defaults and assertions for Rocket's empty incoming stream list.
mlir::LogicalResult addEmptyCPUStreamWrite(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Buffer U250 CPU AXI AW and W independently with depth-two queues.
mlir::LogicalResult addCPUStreamWriteBuffer(circt::firrtl::CircuitOp circuit,
                                          std::string &error);
// Queue U250 CPU AXI AR independently; the engine dequeues on the last R beat.
mlir::LogicalResult addCPUStreamReadBuffer(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Buffer U250 CPU AXI R beats independently from host read backpressure.
mlir::LogicalResult addCPUStreamResponseBuffer(circt::firrtl::CircuitOp circuit,
                                             std::string &error);
// Buffer U250 CPU AXI B responses, preserving the empty sink queue boundary.
mlir::LogicalResult addCPUStreamWriteResponseBuffer(circt::firrtl::CircuitOp circuit,
                                                  std::string &error);
}
#endif
