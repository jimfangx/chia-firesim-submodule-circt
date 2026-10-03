// See LICENSE for license details.
#include "goldengate/SimulatorRTL.h"
#include "circt/Conversion/ExportVerilog.h"
#include "circt/Dialect/Comb/CombDialect.h"
#include "circt/Dialect/Debug/DebugDialect.h"
#include "circt/Dialect/Emit/EmitOps.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "circt/Dialect/OM/OMDialect.h"
#include "circt/Dialect/SV/SVDialect.h"
#include "circt/Dialect/Seq/SeqDialect.h"
#include "circt/Firtool/Firtool.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace circt;

namespace {
// Golden Gate retains source annotations across circuit wrapping. Transfer only
// these module-local sources to the backend clone; replaying the entire archive
// would resolve obsolete targets and repeat Golden Gate transforms.
LogicalResult attachInlineBlackBoxes(firrtl::CircuitOp circuit,
                                    StringRef outputFilename,
                                    std::string &error) {
  auto archive = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!archive)
    return success();
  SymbolTable symbols(circuit);
  llvm::StringMap<StringRef> sources;
  SmallVector<std::pair<firrtl::FExtModuleOp, DictionaryAttr>> pending;
  auto reject = [&](const llvm::Twine &reason) {
    error = reason.str();
    return failure();
  };
  for (Attribute attribute : archive) {
    auto annotation = dyn_cast<DictionaryAttr>(attribute);
    if (!annotation)
      continue;
    auto cls = annotation.getAs<StringAttr>("class");
    if (!cls || cls.getValue() != firrtl::blackBoxInlineAnnoClass)
      continue;
    auto target = annotation.getAs<StringAttr>("target");
    auto name = annotation.getAs<StringAttr>("name");
    auto text = annotation.getAs<StringAttr>("text");
    if (!target || !name || !text || name.getValue().empty() ||
        name.getValue() == "." || name.getValue() == ".." ||
        name.getValue().contains('/') || name.getValue().contains('\\'))
      return reject("inline blackbox requires a module target, local filename "
                    "and source text");
    // SFC ModuleName serializes as Circuit.Module; newer annotations use
    // ~Circuit|Module. Circuit wrapping changes the prefix, not this symbol.
    StringRef moduleName;
    auto value = target.getValue();
    if (value.starts_with("~")) {
      auto split = value.drop_front().split('|');
      if (split.first.empty() || split.second.contains('|') ||
          split.second.contains('>') || split.second.contains('/'))
        return reject("inline blackbox target must identify a module");
      moduleName = split.second;
    } else {
      auto split = value.split('.');
      if (split.first.empty() || split.second.contains('.'))
        return reject("inline blackbox target must identify a module");
      moduleName = split.second;
    }
    auto module =
        dyn_cast_or_null<firrtl::FExtModuleOp>(symbols.lookup(moduleName));
    if (!module)
      return reject("inline blackbox target does not resolve to an external "
                    "module: " + value);
    auto inserted = sources.try_emplace(name.getValue(), text.getValue());
    if (!inserted.second && inserted.first->second != text.getValue())
      return reject("conflicting inline blackbox source filename: " +
                    name.getValue());
    NamedAttrList attached(annotation);
    attached.erase("target");
    pending.emplace_back(module, attached.getDictionary(circuit.getContext()));
  }
  for (auto [module, annotation] : pending) {
    firrtl::AnnotationSet annotations(module);
    if (!llvm::is_contained(annotations.getArray(), annotation))
      annotations.addAnnotations(ArrayRef<Attribute>{annotation});
    annotations.applyToOperation(module);
  }
  if (!pending.empty()) {
    // CIRCT's BlackBoxReader emits unique filenames and the standard resource
    // list. Keep those files beside the candidate RTL for FireSim's append step.
    auto *context = circuit.getContext();
    firrtl::AnnotationSet annotations(circuit);
    auto directory = DictionaryAttr::get(context, {
        {StringAttr::get(context, "class"),
         StringAttr::get(context, firrtl::blackBoxTargetDirAnnoClass)},
        {StringAttr::get(context, "targetDir"),
         StringAttr::get(context, llvm::sys::path::parent_path(outputFilename))}});
    annotations.addAnnotations(ArrayRef<Attribute>{directory});
    annotations.applyToOperation(circuit);
  }
  return success();
}
} // namespace

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
  if (failed(attachInlineBlackBoxes(circuit, outputFilename, error)))
    return failure();
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
      failed(firtool::populateHWToSV(passes, options)))
    return reject("cannot construct CIRCT simulator RTL pipeline");
  if (failed(passes.run(*lowered)))
    return reject("CIRCT simulator RTL lowering failed; see pass diagnostics");

  // The single-file exporter includes emit.file payloads in its stream, even
  // resource lists that are not Verilog. Export those operations separately
  // through CIRCT's file exporter, leaving only simulator RTL in this stream.
  OwningOpRef<ModuleOp> collateral = ModuleOp::create(source.getLoc());
  for (auto &op : llvm::make_early_inc_range(*lowered->getBody()))
    if (isa<emit::FileOp, emit::FileListOp>(op))
      op.moveBefore(collateral->getBody(), collateral->getBody()->end());
  PassManager exportPasses(context);
  if (failed(firtool::populateExportVerilog(exportPasses, options, out)) ||
      failed(exportPasses.run(*lowered)))
    return reject("CIRCT simulator RTL export failed; see pass diagnostics");
  out.flush();
  if (!collateral->getBody()->empty() &&
      failed(exportSplitVerilog(*collateral,
                               llvm::sys::path::parent_path(outputFilename))))
    return reject("CIRCT blackbox collateral export failed; see diagnostics");

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
