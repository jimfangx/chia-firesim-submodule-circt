// See LICENSE for license details.
#pragma once
#include "goldengate/AutoCounterControl.h"
namespace goldengate {
// Selected instrumentation composition: every original CSV widget, followed by
// every synthesized Print widget, followed by the CPU-managed stream engine,
// as registered by FPGATop. Requires both enabled widget types and no other
// bridge constructors. This is a simulator boundary, not an FPGA shell.
// Compose native global control and fully buffered outgoing CPU AXI transport;
// derive both driver constructors from the same complete live allocation.
// Clone/verify/commit: preserve IR, child handles and all strings on failure.
// Successful assembly replaces child handles. Decoder text must be installed
// as print-bridge-decoders.h alongside the standard GET_* guarded header.
mlir::LogicalResult assembleInstrumentationControl(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> csvHosts,
    llvm::ArrayRef<circt::firrtl::FModuleOp> printHosts,
    std::string &header, std::string &decoderHeader,
    std::string &peerMap, std::string &error);
// Post-FAME production adapter: materialize and bind every retained CSV/Print
// constructor, then run the complete composition above. Target model rewriting,
// generated clock tokens, memory bridges and the FPGA shell precede/follow this
// selected boundary; this entry point does not claim full feature acceptance.
mlir::LogicalResult compileInstrumentationControl(
    circt::firrtl::CircuitOp circuit, unsigned addressBits, unsigned idBits,
    std::string &header, std::string &decoderHeader,
    std::string &peerMap, std::string &error);
}
