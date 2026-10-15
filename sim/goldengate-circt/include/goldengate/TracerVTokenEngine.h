// See LICENSE for license details.
#ifndef GOLDENGATE_TRACERVTOKENENGINE_H
#define GOLDENGATE_TRACERVTOKENENGINE_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Map every one-retirement Rocket TracerV HostPort to independent native
// token/stream FIRRTL, preserving constructor order and distinct targets.
// Trigger selection, initDone/traceEnable MMIO and stream storage remain explicit
// control and stream boundaries until their subsequent compiler stages.
mlir::LogicalResult addTracerVTokenEngine(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Attach the same native engine to an explicitly selected active boundary.
// Validate twelve latency-one tokens per bridge, with no shared endpoints,
// generated-name collisions, or unsupported constructors, before any
// mutation. Preserve other ports/modules and their annotation identities.
mlir::LogicalResult attachTracerVTokenEngine(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, std::string &error);
// Connect each engine to its own decoded 15-word Rocket configuration bank, preserving
// host-cycle PC/instruction state and the delayed cycle/PC limit registers.
// Reuse an imported bank only after full operation/metadata equivalence; all
// unsupported or incompatible boundaries reject before mutating the circuit.
mlir::LogicalResult addTracerVTriggerConfig(circt::firrtl::CircuitOp circuit,
                                           std::string &error);
// Buffer each ordered Rocket CPU stream with its own 6144-entry synchronous-read queue.
mlir::LogicalResult addTracerVStreamQueue(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Validate all producers and helper names before attaching independent queues
// to an expanded boundary. Keep producer stream targets
// on the inner module, transfer copied targets, and preserve other host modules.
mlir::LogicalResult attachTracerVStreamQueue(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
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
// Attach the same native queues to an expanded boundary. Preserves existing
// modules and transfers copied targets while retaining consumed AW/W targets.
mlir::LogicalResult attachCPUStreamWriteBuffer(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
// Queue U250 CPU AXI AR independently; the engine dequeues on the last R beat.
mlir::LogicalResult addCPUStreamReadBuffer(circt::firrtl::CircuitOp circuit,
                                         std::string &error);
// Validate the shared U250 AW/AR payload queue before composing later stages.
mlir::LogicalResult validateCPUStreamAddressQueue(circt::firrtl::CircuitOp circuit,
                                                std::string &error);
// Attach the shared native AR queue to an expanded boundary, transferring
// copied targets while retaining consumed AR targets on the inner module.
mlir::LogicalResult attachCPUStreamReadBuffer(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
// Buffer U250 CPU AXI R beats independently from host read backpressure.
mlir::LogicalResult addCPUStreamResponseBuffer(circt::firrtl::CircuitOp circuit,
                                             std::string &error);
// Attach the shared native R queue to an expanded boundary, retaining consumed
// R targets and transferring copied ports to the verified outer wrapper.
mlir::LogicalResult attachCPUStreamResponseBuffer(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
// Buffer U250 CPU AXI B responses, preserving the empty sink queue boundary.
mlir::LogicalResult addCPUStreamWriteResponseBuffer(circt::firrtl::CircuitOp circuit,
                                                  std::string &error);
// Attach the shared native B queue to an expanded boundary, retaining consumed
// B targets and transferring copied targets to the new outer wrapper.
mlir::LogicalResult attachCPUStreamWriteResponseBuffer(circt::firrtl::CircuitOp circuit,
    llvm::StringRef inputBoundary, llvm::StringRef wrapperName, std::string &error);
}
#endif
