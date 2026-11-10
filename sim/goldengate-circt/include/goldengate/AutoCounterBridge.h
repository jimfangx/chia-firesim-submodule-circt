// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "goldengate/ControlAddressDecode.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace goldengate {
// Materialize the actual CSV AutoCounter widget from retained BridgeIO
// AutoCounterParameters: accepted-token accumulation/Identity, delayed periodic
// sampling, trigger/global-reset behavior, depth-two sample queue, decoded MMIO
// and the shared local AXI transport. Constructor order controls widget identity.
// Validate every constructor before mutation; retain annotations. Host-token
// binding, global control allocation and platform composition remain separate.
mlir::LogicalResult materializeAutoCounterBridgeHosts(
    circt::firrtl::CircuitOp circuit, unsigned addressBits, unsigned idBits,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &hosts, std::string &error);

// Bind the complete constructor-ordered host collection to the active FAME
// top's outgoing latency-zero wire-token channels. Join payloads atomically,
// preserve rational clock ownership and expose one indexed local AXI control
// port per widget. Clone/verify/commit preserves IR and annotations on failure.
// Global allocation, driver metadata and platform composition remain separate.
mlir::LogicalResult bindAutoCounterBridgeHosts(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts, std::string &error);

// Discover constructor-ordered bound hosts and validate their actual constructor,
// bank, clock and local control connections. An absent CSV feature returns an
// empty collection; retained constructors or detached materialized hosts fail.
// Caller handles remain valid and output is unchanged on failure.
mlir::LogicalResult discoverBoundAutoCounterBridgeHosts(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<circt::firrtl::FModuleOp> &hosts, std::string &error);

// Shared MCRFile transport adapter; implemented with the existing widget
// transport in ClockBridgeControl.cpp, preserving register lane count.
mlir::LogicalResult materializeAutoCounterBridgeAXI(
    circt::firrtl::CircuitOp circuit, circt::firrtl::FModuleOp bank,
    unsigned addressBits, unsigned idBits, llvm::StringRef wrapperName,
    llvm::StringRef adapterName, circt::firrtl::FModuleOp &host,
    std::string &error);
}

namespace goldengate {
// Derive complete constructor-ordered CSV widget sizes from the bound hosts'
// actual bank and MCRFile. Validate names, permissions, clocks and binding
// order; no IR mutation and preserve caller output on failure.
mlir::LogicalResult deriveAutoCounterControlMMIOCatalog(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    llvm::SmallVectorImpl<ControlMMIOWidget> &widgets, std::string &error);
// Emit real autocounter_t constructors/AddressMap/counter metadata from bound
// CSV hardware and caller-supplied global regions. Regions may include other
// widgets; require exactly one matching region per CSV host, the implemented
// region size and address range. No implicit standalone/global allocation.
// Preserve IR, annotations and caller header on any malformed batch.
mlir::LogicalResult prepareAutoCounterBridgeHeader(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<circt::firrtl::FModuleOp> hosts,
    llvm::ArrayRef<ControlMMIORegion> regions,
    std::string &header, std::string &error);
}
