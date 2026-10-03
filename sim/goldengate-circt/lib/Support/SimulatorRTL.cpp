// See LICENSE for license details.
#include "goldengate/SimulatorRTL.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Debug/DebugDialect.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/OM/OMDialect.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/Seq/SeqDialect.h"
#include "circt/Firtool/Firtool.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace circt;

LogicalResult goldengate::emitSimulatorRTL(ModuleOp source,
                                          StringRef inputFilename,
                                          StringRef outputFilename,
                                          std::string &error) {
  auto reject = [&](StringRef reason) {
    error = reason.str();
    return failure();
  };
  if (outputFilename.empty())
    return reject("simulator RTL requires an output filename");
  auto circuits = source.getOps<firrtl::CircuitOp>();
  if (std::distance(circuits.begin(), circuits.end()) != 1 ||
      failed(verify(source)))
    return reject("simulator RTL requires one valid FIRRTL circuit");

  auto *context = source.getContext();
  bool printOperations = context->shouldPrintOpOnDiagnostic();
  context->printOpOnDiagnostic(false);
  auto restoreDiagnostics = llvm::make_scope_exit(
      [&] { context->printOpOnDiagnostic(printOperations); });
  context->loadDialect<firrtl::FIRRTLDialect, chirrtl::CHIRRTLDialect,
                       hw::HWDialect, comb::CombDialect, seq::SeqDialect,
                       sv::SVDialect, om::OMDialect, debug::DebugDialect>();
  OwningOpRef<ModuleOp> lowered = cast<ModuleOp>(source->clone());
  // rawAnnotations is Golden Gate's serialized archive, including classes
  // already interpreted by its analyses and collateral writers. Replaying it
  // through LowerAnnotations would resolve pre-rewrite targets a second time.
  // Never remove attached operation/port annotations or the source archive.
  auto circuit = *lowered->getOps<firrtl::CircuitOp>().begin();
  circuit->removeAttr("rawAnnotations");

  firtool::FirtoolOptions options;
  options.setOutputFilename(outputFilename)
      .setNoDedup(true)
      .setDisableHoistingHWPassthrough(true)
      .setBlackBoxRootPath(llvm::sys::path::parent_path(outputFilename));
  std::string rtl;
  llvm::raw_string_ostream out(rtl);
  PassManager passes(context);
  if (failed(firtool::populatePreprocessTransforms(passes, options)) ||
      failed(firtool::populateCHIRRTLToLowFIRRTL(passes, options, inputFilename)) ||
      failed(firtool::populateLowFIRRTLToHW(passes, options)) ||
      failed(firtool::populateHWToSV(passes, options)) ||
      failed(firtool::populateExportVerilog(passes, options, out)))
    return reject("cannot construct CIRCT simulator RTL pipeline");
  if (failed(passes.run(*lowered)))
    return reject("CIRCT simulator RTL lowering failed; see pass diagnostics");
  out.flush();

  llvm::SmallString<256> temporary;
  int fd;
  auto ec = llvm::sys::fs::createUniqueFile(outputFilename + ".tmp-%%%%%%",
                                           fd, temporary);
  if (ec)
    return reject("cannot open simulator RTL output: " + ec.message());
  llvm::raw_fd_ostream file(fd, true);
  file << rtl;
  file.close();
  if (file.has_error()) {
    error = "cannot finish simulator RTL output: " + file.error().message();
    file.clear_error();
    llvm::sys::fs::remove(temporary);
    return failure();
  }
  ec = llvm::sys::fs::rename(temporary, outputFilename);
  if (ec) {
    llvm::sys::fs::remove(temporary);
    return reject("cannot publish simulator RTL output: " + ec.message());
  }
  return success();
}
