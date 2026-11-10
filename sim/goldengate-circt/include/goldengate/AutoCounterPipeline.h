// See LICENSE for license details.
#pragma once
#include "goldengate/FAMEModelGraph.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
namespace goldengate {
struct AutoCounterPipelineResult {
  unsigned widgets = 0;
  unsigned printWidgets = 0;
  unsigned printSites = 0;
  unsigned triggerSources = 0;
  unsigned resetSinks = 0;
  FAMEModelGraphResult models;
};
// Compose enabled CSV synthesis, real trigger/global-reset wiring, inferred
// rational bridge clock ownership, standard model preparation and FAME hardware,
// then materialize and bind the actual constructor-ordered CSV widgets.
// Input is scalar LowForm with complete caller-supplied data/clock channels;
// no clocks, token schedules, trigger sources or unspecified payloads are invented.
// External target channels and indexed local AXI controls remain exposed.
// Requires an unwrapped target and the existing FAME graph's supported models.
// Global MMIO/header/platform assembly and Print remain separate.
// Clone/verify/commit is atomic; all child handles change on success.
mlir::LogicalResult compileAutoCounterCSVToBoundHosts(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &error);
// The same source-to-FAME-to-host pipeline followed by the complete CSV-only
// global AXI network. Allocate actual banks with unchanged HasWidgets rules and
// emit constructors from that exact allocation. No independent local AXI buses
// survive. Full platform / Print remains separate. Atomic, including header.
mlir::LogicalResult compileAutoCounterCSVToControl(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &header, std::string &error);
// Enabled source-to-simulator instrumentation boundary. Run CSV selection and
// enabled PrintSynthesis before real trigger/reset/clock wiring and complete
// model FAME. Then bind every retained CSV/Print constructor into one shared
// global control network, buffered CPU transport and allocation-bound driver
// and decoder headers. The scalar LowForm input must contain named selected
// printf operations and complete external data/clock channel annotations.
// Original platform bridges and FPGA-shell assembly remain separate; mixed
// catalogs containing another widget are rejected rather than omitted.
// Atomic: preserve the circuit, result and collateral on every failure.
mlir::LogicalResult compileInstrumentationSourceToControl(
    circt::firrtl::CircuitOp circuit,
    llvm::ArrayRef<llvm::StringRef> coverModuleNames,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &header, std::string &decoderHeader, std::string &peerMap,
    std::string &error);
}
