// See LICENSE for license details.
#ifndef GOLDENGATE_CLOCKBRIDGECONTROL_H
#define GOLDENGATE_CLOCKBRIDGECONTROL_H
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>
namespace goldengate {
// Map the six-word decoded ClockBridge bank to Lib.scala MCRFile transactions.
// The caller supplies the platform CtrlNastiKey address/ID widths; data is 32b.
// Global widget address allocation and the platform control crossbar are pending.
mlir::LogicalResult mapClockBridgeControl(circt::firrtl::CircuitOp circuit,
                                         unsigned addressBits, unsigned idBits,
                                         std::string &error);
// Map ResetPulseBridge's pulseLength/doneInit bank using the same MCRFile
// transport. Address bit 2 selects the word; higher address bits alias.
mlir::LogicalResult mapResetPulseBridgeControl(circt::firrtl::CircuitOp circuit,
                                              unsigned addressBits, unsigned idBits,
                                              std::string &error);
// UART uses the same six-word MCRFile transport as ClockBridge.
mlir::LogicalResult mapUARTBridgeControl(circt::firrtl::CircuitOp circuit,
                                        unsigned addressBits, unsigned idBits,
                                        std::string &error);
// PeekPoke's seven-word bank includes a backpressured STEP sink. Index seven
// aliases read word zero and cannot commit a write, matching SFC MCRFile_1.
mlir::LogicalResult mapPeekPokeBridgeControl(circt::firrtl::CircuitOp circuit,
                                            unsigned addressBits, unsigned idBits,
                                            std::string &error);
// TracerV's fifteen-word configuration bank uses a four-bit local index.
// Index fifteen aliases read word zero and leaves writes uncommitted.
mlir::LogicalResult mapTracerVBridgeControl(circt::firrtl::CircuitOp circuit,
                                           unsigned addressBits, unsigned idBits,
                                           std::string &error);
// Map the ordered CPU stream occupancy bank using its actual MCR vector size.
// A single-word bank retains the Scala behavior where all addresses alias it.
mlir::LogicalResult mapCPUStreamControl(circt::firrtl::CircuitOp circuit,
                                       unsigned addressBits, unsigned idBits,
                                       std::string &error);
// Route LoadMem decoded words 0..8 from its four banks into MCRFile transport.
// Index 9..15 aliases read word zero and leaves writes uncommitted.
mlir::LogicalResult mapLoadMemControl(circt::firrtl::CircuitOp circuit,
                                     unsigned addressBits, unsigned idBits,
                                     std::string &error);
// SimulationMaster has three ReadWrite words; index three aliases read zero
// and cannot commit a write, matching the recorded SFC MCRFile.
mlir::LogicalResult mapSimulationMasterControl(circt::firrtl::CircuitOp circuit,
                                             unsigned addressBits, unsigned idBits,
                                             std::string &error);
// TSI's nine ReadWrite words use addr[5:2]. Indices 9..15 alias read word
// zero and cannot commit writes, matching the recorded SFC MCRFile_7.
mlir::LogicalResult mapTSIBridgeControl(circt::firrtl::CircuitOp circuit,
                                      unsigned addressBits, unsigned idBits,
                                      std::string &error);
// BlockDev's 26-word bank uses addr[6:2]. Indices 26..31 read word zero
// without a lane read handshake and cannot commit writes (SFC MCRFile_3).
mlir::LogicalResult mapBlockDevBridgeControl(circt::firrtl::CircuitOp circuit,
                                           unsigned addressBits, unsigned idBits,
                                           std::string &error);
// FASED's 21-word bank uses five-bit local decode; invalid reads alias word
// zero, invalid writes never commit. Platform slave binding is separate.
mlir::LogicalResult mapFASEDBridgeControl(circt::firrtl::CircuitOp circuit,
                                        unsigned addressBits, unsigned idBits,
                                        std::string &error);

}
#endif
