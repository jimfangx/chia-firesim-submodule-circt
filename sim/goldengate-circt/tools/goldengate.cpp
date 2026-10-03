// See LICENSE for license details.
// The first Golden Gate CIRCT boundary: import SFC's pre-FAME FIRRTL without
// lowering away FireSim annotations, and inventory the CIRCT operations.
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRParser.h"
#include "circt/Dialect/FIRRTL/FIREmitter.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/Pass/PassManager.h"
#include "circt/Dialect/HW/HWDialect.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/AnnotationEmission.h"
#include "goldengate/MetasimInterfaceHeader.h"
#include "goldengate/SimulationMasterHeader.h"
#include "goldengate/ClockBridgeHeader.h"
#include "goldengate/ResetPulseHeader.h"
#include "goldengate/LoadMemHeader.h"
#include "goldengate/PeekPokeHeader.h"
#include "goldengate/UARTHeader.h"
#include "goldengate/TSIHeader.h"
#include "goldengate/BlockDevHeader.h"
#include "goldengate/TracerVHeader.h"
#include "goldengate/CPUManagedStreamHeader.h"
#include "goldengate/FASEDHeader.h"
#include "goldengate/XDCEmission.h"
#include "goldengate/XilinxHostSpecialization.h"
#include "goldengate/SimulatorRTL.h"
#include "goldengate/AutoCounterAnalysis.h"
#include "goldengate/AutoCounterResetGate.h"
#include "goldengate/AutoCounterPrintfValues.h"
#include "goldengate/PrintStubs.h"
#include "goldengate/PrintWiring.h"
#include "goldengate/BridgeAnalysis.h"
#include "goldengate/ChannelAnalysis.h"
#include "goldengate/ChannelClockInfo.h"
#include "goldengate/CombDependencyAnalysis.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEDefaults.h"
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/PromotePassthroughConnections.h"
#include "goldengate/ExtractModel.h"
#include "goldengate/WrapTop.h"
#include "goldengate/LabelMultiThreadedInstances.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/CoerceAsyncToSyncReset.h"
#include "goldengate/ChannelExcision.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FAMEHostControl.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/FAMEOutputValid.h"
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/FAMEReadyValidChannel.h"
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/SingleClockBridge.h"
#include "goldengate/ClockBridgeControl.h"
#include "goldengate/ControlErrorSlave.h"
#include "goldengate/ControlAddressDecode.h"
#include "goldengate/ControlWriteRoute.h"
#include "goldengate/ControlWriteDispatch.h"
#include "goldengate/ControlWidgetWrites.h"
#include "goldengate/ControlReadDispatch.h"
#include "goldengate/ControlReadTracker.h"
#include "goldengate/ControlReadArbiter.h"
#include "goldengate/ControlWriteArbiter.h"
#include "goldengate/ControlWriteTracker.h"
#include "goldengate/SimulationMaster.h"
#include "goldengate/SimulationMasterControl.h"
#include "goldengate/FASEDHostMemory.h"
#include "goldengate/FASEDAddressTranslation.h"
#include "goldengate/FASEDReadDeinterleaver.h"
#include "goldengate/FASEDHostMemoryBuffer.h"
#include "goldengate/HostMemoryWriteArbiter.h"
#include "goldengate/HostMemoryWriteResponses.h"
#include "goldengate/HostMemoryReadArbiter.h"
#include "goldengate/HostMemoryReadResponses.h"
#include "goldengate/HostMemoryOutputBuffer.h"
#include "goldengate/HostMemoryPort.h"
#include "goldengate/CPUStreamPort.h"
#include "goldengate/FPGATopShell.h"
#include "goldengate/F1Shim.h"
#include "goldengate/TSITokenEngine.h"
#include "goldengate/BlockDevTokenEngine.h"
#include "goldengate/BlockDevWriteLatency.h"
#include "goldengate/BlockDevReadLatency.h"
#include "goldengate/BlockDevResponseScheduler.h"
#include "goldengate/FASEDTokenEngine.h"
#include "goldengate/FASEDHostOutstanding.h"
#include "goldengate/FASEDIngressAWQueue.h"
#include "goldengate/FASEDIngressWQueue.h"
#include "goldengate/FASEDIngressARQueue.h"
#include "goldengate/FASEDIngressCredits.h"
#include "goldengate/FASEDIngressIssue.h"
#include "goldengate/FASEDIngressDeadlock.h"
#include "goldengate/FASEDReadBuffer.h"
#include "goldengate/FASEDReadScheduler.h"
#include "goldengate/FASEDWriteEgress.h"
#include "goldengate/FASEDResponseReleaser.h"
#include "goldengate/FASEDTimingCycle.h"
#include "goldengate/FASEDReadLatency.h"
#include "goldengate/FASEDWriteLatency.h"
#include "goldengate/FASEDTimingAWQueue.h"
#include "goldengate/FASEDWritePairing.h"
#include "goldengate/FASEDWriteRetirement.h"
#include "goldengate/FASEDWriteAdmission.h"
#include "goldengate/FASEDReadAdmission.h"
#include "goldengate/FASEDRequestLimits.h"
#include "goldengate/FASEDLatencyRegisters.h"
#include "goldengate/FASEDFunctionalModelRegister.h"
#include "goldengate/FASEDResponseErrors.h"
#include "goldengate/FASEDStatistics.h"
#include "goldengate/FASEDHistograms.h"
#include "goldengate/FASEDMMIOBank.h"
#include "goldengate/FASEDIngressOrder.h"
#include "goldengate/BlockDevRequestQueue.h"
#include "goldengate/BlockDevDataQueue.h"
#include "goldengate/BlockDevReadResponseQueue.h"
#include "goldengate/BlockDevWriteAckQueue.h"
#include "goldengate/BlockDevMMIOBank.h"
#include "goldengate/TSIWordQueues.h"
#include "goldengate/TSIMMIOBank.h"
#include "goldengate/ResetPulseBridge.h"
#include "goldengate/PeekPokeCycleEngine.h"
#include "goldengate/TracerVTokenEngine.h"
#include "goldengate/LoadMemWriter.h"
#include "goldengate/UARTSerialEngine.h"
#include "goldengate/HierarchyAnalysis.h"
#include "goldengate/ModelAnalysis.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/TriggerWiring.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Support/Timing.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <set>
#include <string>
#include <tuple>

using namespace circt;
using namespace circt::firrtl;

static int fail(llvm::StringRef message) {
  llvm::errs() << "goldengate-circt: " << message << '\n';
  return 1;
}

int main(int argc, char **argv) {
  bool rewriteOutputValids =
      argc == 8 &&
      llvm::StringRef(argv[6]) == "--rewrite-fame-output-valid-from";
  bool addHostControl = argc == 8 &&
                        llvm::StringRef(argv[6]) == "--add-fame-host-control-to";
  bool rewriteInputChannel =
      argc == 8 && llvm::StringRef(argv[6]) == "--rewrite-fame-input-channel";
  bool rewriteOutputChannel =
      argc == 8 && llvm::StringRef(argv[6]) == "--rewrite-fame-output-channel";
  bool rewriteInputsWithOutput =
      argc == 8 && llvm::StringRef(argv[6]) == "--rewrite-fame-inputs-with-output";
  bool applyFAMEDefaults =
      argc == 7 && llvm::StringRef(argv[6]) == "--apply-fame-defaults";
  bool promotePassthrough =
      argc == 7 && llvm::StringRef(argv[6]) == "--promote-passthrough";
  bool extractModels =
      argc == 7 && llvm::StringRef(argv[6]) == "--extract-models";
  bool wrapTop = argc == 7 && llvm::StringRef(argv[6]) == "--wrap-top";
  bool updateBridgeClocks =
      argc == 7 && llvm::StringRef(argv[6]) == "--update-bridge-clocks";
  bool labelMultiThreaded =
      argc == 7 &&
      (llvm::StringRef(argv[6]) == "--label-multithreaded-models=on" ||
       llvm::StringRef(argv[6]) == "--label-multithreaded-models=off");
  bool inferDefaultClocks =
      argc == 7 && llvm::StringRef(argv[6]) == "--infer-default-clocks";
  bool exciseChannels =
      argc == 7 && llvm::StringRef(argv[6]) == "--excise-channels";
  bool inferModelPorts =
      argc == 7 && llvm::StringRef(argv[6]) == "--infer-model-ports";
  bool promoteGroundBridges =
      argc == 7 && llvm::StringRef(argv[6]) == "--promote-ground-bridges";
  bool promoteAggregateBridges =
      argc == 7 && llvm::StringRef(argv[6]) == "--promote-aggregate-bridges";
  bool resolveDontTouch =
      argc == 7 && llvm::StringRef(argv[6]) == "--resolve-dont-touch";
  bool lowerTypes =
      argc == 7 && llvm::StringRef(argv[6]) == "--lower-types";
  bool analyzeAutoCounter =
      argc == 7 && llvm::StringRef(argv[6]) == "--analyze-autocounter";
  bool gateAutoCounter =
      argc == 7 && llvm::StringRef(argv[6]) == "--gate-autocounter-events";
  bool gateSelectedAutoCounter =
      argc == 7 && llvm::StringRef(argv[6]) == "--gate-selected-autocounter-events";
  bool synthesizeAutoCounterValues =
      argc == 7 && llvm::StringRef(argv[6]) == "--synthesize-autocounter-printf-values";
  bool analyzeAutoCounterPrintClocks =
      argc == 7 && llvm::StringRef(argv[6]) == "--analyze-autocounter-print-clocks";
  bool wireAutoCounterStubs = analyzeAutoCounterPrintClocks ||
      (argc == 7 && llvm::StringRef(argv[6]) == "--wire-autocounter-print-stubs");
  bool wirePrintStubs =
      argc == 7 && llvm::StringRef(argv[6]) == "--wire-print-stubs";
  bool synthesizeAutoCounterStubs =
      wireAutoCounterStubs || (argc == 7 && llvm::StringRef(argv[6]) == "--synthesize-autocounter-print-stubs");
  bool synthesizePrintStubs =
      wirePrintStubs || (argc == 7 && llvm::StringRef(argv[6]) == "--synthesize-print-stubs");
  bool synthesizeAutoCounterPrints =
      synthesizeAutoCounterStubs ||
      (argc == 7 && llvm::StringRef(argv[6]) == "--synthesize-autocounter-printf");
  bool disableAutoCounter =
      argc == 7 && llvm::StringRef(argv[6]) == "--disable-autocounter";
  bool compileBaseline =
      (argc == 7 || (argc == 9 && llvm::StringRef(argv[7]) == "--output-filename-base")) &&
      llvm::StringRef(argv[6]) == "--compile-baseline";
  if ((argc != 6 && !rewriteOutputValids && !addHostControl &&
       !rewriteInputChannel && !rewriteOutputChannel &&
       !rewriteInputsWithOutput && !applyFAMEDefaults &&
       !promotePassthrough && !extractModels && !wrapTop &&
       !updateBridgeClocks &&
       !labelMultiThreaded &&
       !inferDefaultClocks && !exciseChannels && !inferModelPorts &&
       !promoteGroundBridges && !promoteAggregateBridges &&
       !resolveDontTouch && !lowerTypes && !analyzeAutoCounter &&
       !gateAutoCounter && !gateSelectedAutoCounter && !synthesizeAutoCounterValues && !synthesizeAutoCounterPrints &&
       !synthesizePrintStubs && !disableAutoCounter && !compileBaseline) ||
      llvm::StringRef(argv[2]) != "--annotation-file" ||
      llvm::StringRef(argv[4]) != "--output-dir") {
    llvm::errs() << "usage: goldengate-circt input.fir --annotation-file "
                    "input.json --output-dir directory "
                    "[--rewrite-fame-output-valid-from analysis.json | "
                    "--add-fame-host-control-to module | "
                    "--rewrite-fame-input-channel all|channel[,channel...] | "
                    "--rewrite-fame-output-channel channel | "
                    "--rewrite-fame-inputs-with-output all|channel[,channel...] | "
                    "--apply-fame-defaults | --promote-passthrough | "
                    "--extract-models | --wrap-top | --update-bridge-clocks | "
                    "--label-multithreaded-models=on|off | "
                    "--infer-default-clocks | "
                    "--excise-channels | --infer-model-ports | "
                    "--promote-ground-bridges | "
                    "--promote-aggregate-bridges | --resolve-dont-touch | "
                    "--lower-types | --analyze-autocounter | --gate-autocounter-events | "
                    "--gate-selected-autocounter-events | "
                    "--synthesize-autocounter-printf-values | --synthesize-autocounter-printf | "
                    "--synthesize-print-stubs | --synthesize-autocounter-print-stubs | "
                    "--wire-print-stubs | --wire-autocounter-print-stubs | "
                    "--analyze-autocounter-print-clocks | "
                    "--disable-autocounter | --compile-baseline "
                    "[--output-filename-base name]]\n";
    return 2;
  }

  llvm::StringRef firPath(argv[1]), annoPath(argv[3]), outputDir(argv[5]);
  llvm::StringRef outputBase = compileBaseline && argc == 9
                                  ? argv[8] : "FireSim-generated";
  if (auto error = llvm::sys::fs::create_directories(outputDir))
    return fail("cannot create output directory: " + error.message());

  llvm::SourceMgr sourceMgr;
  auto firBuffer = llvm::MemoryBuffer::getFile(firPath);
  if (!firBuffer)
    return fail("cannot read FIRRTL file: " + firPath.str());
  sourceMgr.AddNewSourceBuffer(std::move(*firBuffer), llvm::SMLoc());
  auto annoBuffer = llvm::MemoryBuffer::getFile(annoPath);
  if (!annoBuffer)
    return fail("cannot read annotation file: " + annoPath.str());
  auto parsedAnnotations = llvm::json::parse((*annoBuffer)->getBuffer());
  if (!parsedAnnotations || !parsedAnnotations->getAsArray())
    return fail("annotation file must contain a JSON array");
  sourceMgr.AddNewSourceBuffer(std::move(*annoBuffer), llvm::SMLoc());

  mlir::DialectRegistry registry;
  registry.insert<FIRRTLDialect, chirrtl::CHIRRTLDialect, hw::HWDialect>();
  mlir::MLIRContext context(registry);
  mlir::DefaultTimingManager timer;
  auto scope = timer.getRootScope();
  FIRParserOptions options;
  options.numAnnotationFiles = 1;
  auto module = importFIRFile(sourceMgr, &context, scope, options);
  if (!module || mlir::failed(mlir::verify(*module)))
    return fail("FIRRTL/annotation import failed");

  CircuitOp circuit;
  module->walk([&](CircuitOp op) { circuit = op; });
  if (!circuit)
    return fail("input did not produce a firrtl.circuit");
  const std::string originalTargetName = circuit.getName().str();

  if (disableAutoCounter || compileBaseline) {
    std::string error;
    if (compileBaseline) {
      // BridgeExtraction precedes the second SFC low-form lowering. Keep this
      // boundary in the normal CIRCT path as well: LowerTypes would otherwise
      // erase the aggregate bridge channel identities needed here.
      if (mlir::failed(goldengate::promoteBridgePorts(circuit, error, true)))
        return fail("BridgeExtraction: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("BridgeExtraction produced invalid FIRRTL IR");
      llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
      llvm::sys::path::append(firPath, "post-bridge-extraction.fir");
      llvm::sys::path::append(annotationPath,
                              "post-bridge-extraction-all.json");
      std::error_code writeError;
      llvm::raw_fd_ostream firOut(firPath, writeError);
      if (writeError)
        return fail("cannot write BridgeExtraction FIRRTL: " +
                    writeError.message());
      if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                    exportFIRVersion)))
        return fail("cannot export BridgeExtraction FIRRTL");
      firOut.close();
      if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                      error)))
        return fail("cannot export BridgeExtraction annotations: " + error);
      llvm::outs() << "Promoted CIRCT bridges in " << firPath << '\n';

      // Resolve clock domains while bridge endpoints still have aggregate
      // FIRRTL identities. The SFC computes the same metadata later in its
      // pipeline; retaining it here lets LowerTypes carry it forward.
      if (mlir::failed(goldengate::analyzeChannelClocksAndUpdateBridges(
              circuit, error)))
        return fail("ChannelClockInfo/UpdateBridgeClockInfo: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("bridge clock analysis produced invalid FIRRTL IR");
      llvm::SmallString<256> clockAnnotationPath(outputDir);
      llvm::sys::path::append(clockAnnotationPath,
                              "post-bridge-clocks-all.json");
      if (mlir::failed(goldengate::emitAllAnnotations(
              circuit, clockAnnotationPath, error)))
        return fail("cannot export bridge clock annotations: " + error);
      llvm::outs() << "Resolved CIRCT bridge clocks in "
                   << clockAnnotationPath << '\n';
    }
    if (mlir::failed(goldengate::lowerTypesWithRetainedTargets(
            *module, circuit, error)))
      return fail("AutoCounter LowerTypes: " + error);
    if (compileBaseline) {
      // SFC normalizes target resets before FAME introduces gated clocks.
      goldengate::coerceAsyncToSyncReset(circuit);
      llvm::outs() << "Coerced CIRCT target resets to synchronous Bool resets\n";
    }
    unsigned removed = 0;
    if (mlir::failed(goldengate::dropDisabledAutoCounterAnnotations(
            circuit, removed, error)))
      return fail("AutoCounter cleanup: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("disabled AutoCounter pipeline produced invalid FIRRTL IR");
    llvm::SmallString<256> irPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(irPath, "post-autocounter.mlir");
    llvm::sys::path::append(annotationPath, "post-autocounter-all.json");
    std::error_code writeError;
    llvm::raw_fd_ostream irOut(irPath, writeError);
    if (writeError)
      return fail("cannot write post-AutoCounter MLIR: " +
                  writeError.message());
    module->print(irOut);
    irOut << '\n';
    irOut.close();
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("cannot export post-AutoCounter annotations: " + error);
    llvm::outs() << "Dropped " << removed
                 << " disabled AutoCounter annotations in " << annotationPath
                 << "; lowered FIRRTL in " << irPath << '\n';
    if (compileBaseline) {
      unsigned consumedTriggerSources = 0;
      if (mlir::failed(goldengate::consumeUnobservedTriggerSources(
              circuit, consumedTriggerSources, error)))
        return fail("TriggerWiring: " + error);
      llvm::SmallString<256> triggerAnnotationPath(outputDir);
      llvm::sys::path::append(triggerAnnotationPath,
                              "post-trigger-wiring.json");
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, triggerAnnotationPath, error)))
        return fail("cannot export trigger wiring annotations: " + error);
      llvm::outs() << "Consumed " << consumedTriggerSources
                   << " unwired CIRCT trigger sources in "
                   << triggerAnnotationPath << '\n';

      if (mlir::failed(goldengate::wrapTop(circuit, error)))
        return fail("WrapTop: " + error);
      // The base Golden Gate configuration leaves model multithreading off.
      // Scala still runs this pass, consuming candidate threading annotations
      // before its post-wrap boundary.
      if (mlir::failed(goldengate::labelMultiThreadedInstances(
              circuit, false, error)))
        return fail("LabelMultiThreadedInstances: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("WrapTop produced invalid FIRRTL IR");
      llvm::SmallString<256> wrapIRPath(outputDir), wrapAnnotationPath(outputDir);
      llvm::sys::path::append(wrapIRPath, "post-wrap-top.mlir");
      llvm::sys::path::append(wrapAnnotationPath, "post-wrap-top-all.json");
      std::error_code wrapWriteError;
      llvm::raw_fd_ostream irOut(wrapIRPath, wrapWriteError);
      if (wrapWriteError)
        return fail("cannot write wrapped MLIR: " +
                    wrapWriteError.message());
      module->print(irOut);
      irOut << '\n';
      irOut.close();
      if (mlir::failed(goldengate::emitAllAnnotations(
              circuit, wrapAnnotationPath, error)))
        return fail("cannot export wrapped annotations: " + error);
      llvm::outs() << "Wrapped CIRCT top in " << wrapIRPath << '\n';

      unsigned promotedModels = 0;
      if (mlir::failed(goldengate::extractModels(circuit, promotedModels,
                                               error)))
        return fail("ExtractModel: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("ExtractModel produced invalid FIRRTL IR");
      llvm::SmallString<256> modelIRPath(outputDir), modelAnnotationPath(outputDir);
      llvm::sys::path::append(modelIRPath, "post-extract-model.mlir");
      llvm::sys::path::append(modelAnnotationPath,
                              "post-extract-model-all.json");
      std::error_code modelWriteError;
      llvm::raw_fd_ostream modelOut(modelIRPath, modelWriteError);
      if (modelWriteError)
        return fail("cannot write extracted-model MLIR: " +
                    modelWriteError.message());
      module->print(modelOut);
      modelOut << '\n';
      modelOut.close();
      if (mlir::failed(goldengate::emitAllAnnotations(
              circuit, modelAnnotationPath, error)))
        return fail("cannot export extracted-model annotations: " + error);
      llvm::outs() << "Promoted " << promotedModels << " CIRCT models in "
                   << modelIRPath << '\n';

      unsigned promotedConnections = 0;
      if (mlir::failed(goldengate::promotePassthroughConnections(
              circuit, promotedConnections, error)))
        return fail("PromotePassthroughConnections: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("PromotePassthroughConnections produced invalid FIRRTL IR");
      llvm::SmallString<256> passthroughIRPath(outputDir);
      llvm::SmallString<256> passthroughAnnotationPath(outputDir);
      llvm::sys::path::append(passthroughIRPath,
                              "post-promote-passthrough.mlir");
      llvm::sys::path::append(passthroughAnnotationPath,
                              "post-promote-passthrough.json");
      std::error_code passthroughWriteError;
      llvm::raw_fd_ostream passthroughOut(passthroughIRPath,
                                           passthroughWriteError);
      if (passthroughWriteError)
        return fail("cannot write promoted passthrough MLIR: " +
                    passthroughWriteError.message());
      module->print(passthroughOut);
      passthroughOut << '\n';
      passthroughOut.close();
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, passthroughAnnotationPath, error)))
        return fail("cannot export passthrough annotations: " + error);
      llvm::outs() << "Promoted " << promotedConnections
                   << " CIRCT passthrough connections in "
                   << passthroughIRPath << '\n';

      if (mlir::failed(goldengate::addFAMEDefaults(circuit, error)))
        return fail("FAMEDefaults: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("FAMEDefaults produced invalid FIRRTL IR");
      llvm::SmallString<256> defaultsIRPath(outputDir);
      llvm::SmallString<256> defaultsAnnotationPath(outputDir);
      llvm::sys::path::append(defaultsIRPath, "post-fame-defaults.mlir");
      llvm::sys::path::append(defaultsAnnotationPath,
                              "post-fame-defaults.json");
      std::error_code defaultsWriteError;
      llvm::raw_fd_ostream defaultsOut(defaultsIRPath, defaultsWriteError);
      if (defaultsWriteError)
        return fail("cannot write FAMEDefaults MLIR: " +
                    defaultsWriteError.message());
      module->print(defaultsOut);
      defaultsOut << '\n';
      defaultsOut.close();
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, defaultsAnnotationPath, error)))
        return fail("cannot export FAMEDefaults annotations: " + error);
      llvm::outs() << "Applied CIRCT FAMEDefaults in "
                   << defaultsIRPath << '\n';

      if (mlir::failed(goldengate::findDefaultClocks(circuit, error)))
        return fail("FindDefaultClocks: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("FindDefaultClocks produced invalid FIRRTL IR");
      llvm::SmallString<256> inferredClockAnnotationPath(outputDir);
      llvm::sys::path::append(inferredClockAnnotationPath,
                              "post-find-default-clocks.json");
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, inferredClockAnnotationPath, error)))
        return fail("cannot export inferred clock annotations: " + error);
      llvm::outs() << "Inferred CIRCT model channel clocks in "
                   << inferredClockAnnotationPath << '\n';

      if (mlir::failed(goldengate::exciseChannels(circuit, error)))
        return fail("ChannelExcision: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("ChannelExcision produced invalid FIRRTL IR");
      llvm::SmallString<256> excisionIRPath(outputDir);
      llvm::SmallString<256> excisionAnnotationPath(outputDir);
      llvm::sys::path::append(excisionIRPath, "post-channel-excision.mlir");
      llvm::sys::path::append(excisionAnnotationPath,
                              "post-channel-excision.json");
      std::error_code excisionWriteError;
      llvm::raw_fd_ostream excisionOut(excisionIRPath, excisionWriteError);
      if (excisionWriteError)
        return fail("cannot write ChannelExcision MLIR: " +
                    excisionWriteError.message());
      module->print(excisionOut);
      excisionOut << '\n';
      excisionOut.close();
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, excisionAnnotationPath, error)))
        return fail("cannot export ChannelExcision annotations: " + error);
      llvm::outs() << "Excised CIRCT channels in " << excisionIRPath << '\n';

      if (mlir::failed(goldengate::inferModelPorts(circuit, error)))
        return fail("InferModelPorts: " + error);
      if (mlir::failed(mlir::verify(*module)))
        return fail("InferModelPorts produced invalid FIRRTL IR");
      llvm::SmallString<256> inferredPortsIRPath(outputDir);
      llvm::SmallString<256> inferredPortsAnnotationPath(outputDir);
      llvm::sys::path::append(inferredPortsIRPath,
                              "post-infer-model-ports.mlir");
      llvm::sys::path::append(inferredPortsAnnotationPath,
                              "post-infer-model-ports.json");
      std::error_code inferredPortsWriteError;
      llvm::raw_fd_ostream inferredPortsOut(inferredPortsIRPath,
                                           inferredPortsWriteError);
      if (inferredPortsWriteError)
        return fail("cannot write InferModelPorts MLIR: " +
                    inferredPortsWriteError.message());
      module->print(inferredPortsOut);
      inferredPortsOut << '\n';
      inferredPortsOut.close();
      if (mlir::failed(goldengate::emitFAMEAnnotations(
              circuit, inferredPortsAnnotationPath, error)))
        return fail("cannot export InferModelPorts annotations: " + error);
      llvm::outs() << "Inferred CIRCT model ports in "
                   << inferredPortsIRPath << '\n';

      // Trace selected outputs while their payload ports still have
      // their original FIRRTL identities. Include the input bindings so a
      // missing dependency cannot be mistaken for an independent output.
      std::map<std::string, goldengate::LocalChannelDependency> outputDependencies;
      {
        auto dependencyHierarchy = goldengate::analyzeTopHierarchy(circuit, error);
        if (!dependencyHierarchy)
          return fail("FAME output dependency hierarchy: " + error);
        auto dependencyAnnotations =
            circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
        llvm::SmallVector<goldengate::ModelPortGroup> dependencyGroups;
        for (auto attr : dependencyAnnotations) {
          Annotation annotation(attr);
          if (!annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
            continue;
          auto group = goldengate::analyzeModelPortGroup(circuit, annotation,
                                                        error);
          if (!group)
            return fail("FAME dependency model ports: " + error);
          dependencyGroups.push_back(std::move(*group));
        }
        llvm::SmallVector<goldengate::ModelChannelBinding> dependencyBindings;
        FModuleOp dependencyModel;
        for (auto attr : dependencyAnnotations) {
          Annotation annotation(attr);
          if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
            continue;
          auto name = annotation.getMember<mlir::StringAttr>("globalName");
          if (!name || (name.getValue() != "peekPokeBridge_reset" &&
                        name.getValue() != "resetBridge_reset" &&
                        name.getValue() != "ep_bdev_info_max_req_len" &&
                        name.getValue() != "ep_bdev_info_nsectors" &&
                        name.getValue() != "ep_bdev_data_rev" &&
                        name.getValue() != "ep_bdev_req_rev" &&
                        name.getValue() != "ep_bdev_resp_fwd" &&
                        name.getValue() != "ep_bdev_data_fwd" &&
                        name.getValue() != "ep_bdev_req_fwd" &&
                        name.getValue() != "ep_bdev_resp_rev" &&
                        name.getValue() != "ep_reset" &&
                        name.getValue() != "ep_1_reset" &&
                        name.getValue() != "ep_1_uart_rxd" &&
                        name.getValue() != "ep_1_uart_txd" &&
                        name.getValue() != "ep_2_reset" &&
                        name.getValue() != "ep_3_reset" &&
                        name.getValue() != "ep_3_tsi_in_fwd" &&
                        name.getValue() != "ep_3_tsi_out_rev" &&
                        name.getValue() != "ep_3_tsi_in_rev" &&
                        name.getValue() != "ep_3_tsi_out_fwd" &&
                        name.getValue() != "ep_2_axi4_ar_fwd" &&
                        name.getValue() != "ep_2_axi4_w_fwd" &&
                        name.getValue() != "ep_2_axi4_aw_fwd" &&
                        name.getValue() != "ep_2_axi4_r_rev" &&
                        name.getValue() != "ep_2_axi4_b_rev" &&
                        name.getValue() != "tracerv_tiletrace_reset" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_valid" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_iaddr" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_insn" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_priv" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_exception" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_interrupt" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_cause" &&
                        name.getValue() !=
                            "tracerv_tiletrace_trace_retiredinsns_0_tval" &&
                        name.getValue() != "tracerv_tiletrace_trace_time"))
            continue;
          auto channel = goldengate::analyzeChannelConnection(
              circuit, annotation, error);
          if (!channel)
            return fail("FAME dependency channel: " + error);
          auto bindings = goldengate::bindChannelToModels(
              *channel, *dependencyHierarchy, dependencyGroups, error);
          if (!bindings || bindings->size() != 1)
            return fail("FAME dependency model binding: " + error);
          if (name.getValue() == "ep_bdev_data_fwd") {
            auto boundModule = bindings->front().portGroup->module;
            dependencyModel = dyn_cast<FModuleOp>(boundModule.getOperation());
          }
          dependencyBindings.push_back(std::move(bindings->front()));
        }
        if (!dependencyModel || dependencyBindings.size() != 35)
          return fail("FAME output dependency channels are incomplete");
        auto dependencies = goldengate::analyzeLocalChannelDependencies(
            dependencyModel, dependencyBindings);
        for (auto &dependency : dependencies)
          if (dependency.outputChannel == "ep_bdev_data_fwd" ||
              dependency.outputChannel == "ep_bdev_req_fwd" ||
              dependency.outputChannel == "ep_bdev_resp_ready" ||
              dependency.outputChannel == "ep_reset" ||
              dependency.outputChannel == "ep_1_reset" ||
              dependency.outputChannel == "ep_1_uart_txd" ||
              dependency.outputChannel == "ep_2_reset" ||
              dependency.outputChannel == "ep_3_reset" ||
              dependency.outputChannel == "ep_3_tsi_in_ready" ||
              dependency.outputChannel == "ep_3_tsi_out_fwd" ||
              dependency.outputChannel == "ep_2_axi4_ar_fwd" ||
              dependency.outputChannel == "ep_2_axi4_w_fwd" ||
              dependency.outputChannel == "ep_2_axi4_aw_fwd" ||
              dependency.outputChannel == "ep_2_axi4_r_ready" ||
              dependency.outputChannel == "ep_2_axi4_b_ready" ||
              dependency.outputChannel == "tracerv_tiletrace_reset" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_valid" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_iaddr" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_insn" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_priv" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_exception" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_interrupt" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_cause" ||
              dependency.outputChannel ==
                  "tracerv_tiletrace_trace_retiredinsns_0_tval" ||
              dependency.outputChannel == "tracerv_tiletrace_trace_time")
            outputDependencies.emplace(dependency.outputChannel,
                                       std::move(dependency));
        if (outputDependencies.size() != 25)
          return fail("FAME output dependencies were not found");
        for (const auto &[name, dependency] : outputDependencies)
          if (!dependency.unresolvedPorts.empty() ||
              !dependency.unresolvedCauses.empty()) {
            std::string detail = "FAME output " + name +
                                 " has unresolved combinational dependencies:";
            for (const auto &port : dependency.unresolvedPorts)
              detail += " port=" + port;
            for (const auto &cause : dependency.unresolvedCauses)
              detail += " cause=" + cause;
            return fail(detail);
          }
      }

      auto currentAnnotations =
          circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
      if (!currentAnnotations)
        return fail("FAME host control needs retained annotations");
      std::set<mlir::Operation *> preparedModels;
      for (auto attr : currentAnnotations) {
        Annotation annotation(attr);
        if (!annotation.isClass(goldengate::AnnotationClasses::FAMETransform))
          continue;
        auto target = annotation.getMember<mlir::StringAttr>("target");
        if (!target)
          return fail("FAME transform annotation has no module target");
        std::string targetError;
        auto resolved = goldengate::resolveAnnotationTarget(
            circuit, target.getValue(), targetError);
        if (!resolved || resolved->port)
          return fail("invalid FAME model target: " + targetError);
        auto model = mlir::dyn_cast<FModuleOp>(resolved->module.getOperation());
        if (!model)
          return fail("FAME model target is not an internal module");
        if (!preparedModels.insert(model.getOperation()).second)
          continue;
        if (mlir::failed(goldengate::addFAMEHostControl(circuit, model,
                                                        targetError)))
          return fail("FAME host control: " + targetError);
      }
      if (mlir::failed(mlir::verify(*module)))
        return fail("FAME host control produced invalid FIRRTL IR");
      llvm::SmallString<256> hostControlIRPath(outputDir);
      llvm::sys::path::append(hostControlIRPath, "post-fame-host-control.mlir");
      std::error_code hostControlWriteError;
      llvm::raw_fd_ostream hostControlOut(hostControlIRPath,
                                         hostControlWriteError);
      if (hostControlWriteError)
        return fail("cannot write FAME host-control MLIR: " +
                    hostControlWriteError.message());
      module->print(hostControlOut);
      hostControlOut << '\n';
      hostControlOut.close();
      llvm::outs() << "Added CIRCT FAME host controls in "
                   << hostControlIRPath << '\n';

      // The clock channel is the first FAME input channel. Resolve it from
      // retained targets after InferModelPorts and replace the scalar clock
      // port with the decoupled channel on both the wrapper and model.
      auto clockHierarchy = goldengate::analyzeTopHierarchy(circuit, error);
      if (!clockHierarchy)
        return fail("FAME clock hierarchy: " + error);
      std::optional<goldengate::GGChannelConnection> clockChannel;
      std::optional<goldengate::ModelPortGroup> clockGroup;
      for (auto attr : currentAnnotations) {
        Annotation annotation(attr);
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          auto candidate = goldengate::analyzeChannelConnection(circuit, annotation,
                                                                error);
          if (!candidate)
            return fail("FAME clock channel: " + error);
          if (candidate->kind == goldengate::ChannelKind::TargetClock) {
            if (clockChannel)
              return fail("multiple target clock channels");
            clockChannel = std::move(*candidate);
          }
        } else if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts)) {
          auto candidate = goldengate::analyzeModelPortGroup(circuit, annotation,
                                                              error);
          if (!candidate)
            return fail("FAME clock model ports: " + error);
          if (candidate->ports.size() == 1 &&
              isa<ClockType>(candidate->module.getPorts()[candidate->ports[0]].type) &&
              !candidate->clockPort) {
            if (clockGroup)
              return fail("multiple target clock model groups");
            clockGroup = std::move(*candidate);
          }
        }
      }
      if (!clockChannel || !clockGroup)
        return fail("FAME target clock channel or model group is missing");
      llvm::SmallVector<goldengate::ModelPortGroup> clockGroups;
      clockGroups.push_back(*clockGroup);
      auto clockBindings = goldengate::bindChannelToModels(
          *clockChannel, *clockHierarchy, clockGroups, error);
      if (!clockBindings || clockBindings->size() != 1)
        return fail("FAME clock model binding: " + error);
      llvm::SmallVector<FModuleLike> clockModels;
      clockModels.push_back(clockGroup->module);
      llvm::SmallVector<goldengate::GGChannelConnection, 0> clockChannels;
      clockChannels.push_back(*clockChannel);
      auto clockPlan = goldengate::analyzeFAMEPorts(
          *clockHierarchy, *clockBindings, clockChannels, clockModels, error);
      if (!clockPlan || clockPlan->sinks.size() != 1)
        return fail("FAME clock port plan: " + error);
      auto oldTopClock = clockChannel->sinks.front().module.getPortName(
          *clockChannel->sinks.front().port).str();
      auto oldModelClock = clockGroup->module.getPortName(clockGroup->ports[0]).str();
      auto newTopClock = clockPlan->sinks.front().portName;
      auto newModelClock = clockGroup->name + "_sink";
      if (failed(goldengate::rewriteFAMEInputChannel(
              *clockHierarchy, clockPlan->sinks.front(), error)))
        return fail("FAME clock input channel: " + error);
      auto renameClock = [&](Annotation &annotation, llvm::StringRef member,
                             llvm::StringRef from, llvm::StringRef to) {
        auto entries = annotation.getMember<mlir::ArrayAttr>(member);
        if (!entries)
          return 0u;
        unsigned count = 0;
        llvm::SmallVector<mlir::Attribute> replaced;
        for (auto entry : entries) {
          auto value = dyn_cast<mlir::StringAttr>(entry);
          if (value && value.getValue() == from) {
            replaced.push_back(mlir::StringAttr::get(&context, to));
            ++count;
          } else {
            replaced.push_back(entry);
          }
        }
        if (count)
          annotation.setMember(member, mlir::ArrayAttr::get(&context, replaced));
        return count;
      };
      std::string prefix = "~" + circuit.getName().str() + "|";
      auto topName = clockHierarchy->top.getName().str();
      auto modelName = clockGroup->module.getName().str();
      llvm::SmallVector<mlir::Attribute> clockAnnotations;
      unsigned topRenames = 0, modelRenames = 0;
      for (auto attr : currentAnnotations) {
        Annotation annotation(attr);
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
          topRenames += renameClock(annotation, "sinks",
                                    prefix + topName + ">" + oldTopClock,
                                    prefix + topName + ">" + newTopClock + ".bits");
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
          modelRenames += renameClock(annotation, "ports",
                                      prefix + modelName + ">" + oldModelClock,
                                      prefix + modelName + ">" + newModelClock + ".bits");
        clockAnnotations.push_back(annotation.getAttr());
      }
      if (topRenames != 1 || modelRenames != 1)
        return fail("FAME clock channel annotation targets were not unique");
      circuit->setAttr("rawAnnotations",
                       mlir::ArrayAttr::get(&context, clockAnnotations));
      if (failed(mlir::verify(*module)))
        return fail("FAME clock channel produced invalid FIRRTL IR");
      llvm::SmallString<256> clockIRPath(outputDir);
      llvm::SmallString<256> clockAnnotationPath(outputDir);
      llvm::sys::path::append(clockIRPath, "post-fame-clock-channel.mlir");
      llvm::sys::path::append(clockAnnotationPath,
                              "post-fame-clock-channel.json");
      std::error_code clockWriteError;
      llvm::raw_fd_ostream clockOut(clockIRPath, clockWriteError);
      if (clockWriteError)
        return fail("cannot write FAME clock-channel MLIR: " +
                    clockWriteError.message());
      module->print(clockOut);
      clockOut << '\n';
      clockOut.close();
      if (failed(goldengate::emitFAMEAnnotations(
              circuit, clockAnnotationPath, error)))
        return fail("cannot export FAME clock annotations: " + error);
      llvm::outs() << "Channelized CIRCT FAME target clock in "
                   << clockIRPath << '\n';

      // FAME latches each target clock's incoming token on a finishing host
      // cycle. This register controls the later abstract clock gate.
      auto clockModel = dyn_cast<FModuleOp>(clockGroup->module.getOperation());
      if (!clockModel)
        return fail("FAME target clock model is not an internal module");
      std::optional<unsigned> channelPort;
      for (unsigned i = 0; i < clockModel.getNumPorts(); ++i)
        if (clockModel.getPortName(i) == newModelClock)
          channelPort = i;
      if (!channelPort)
        return fail("FAME target clock channel port is missing");
      mlir::OpBuilder clockBuilder(&clockModel.getBodyBlock()->front());
      auto clockBits = clockBuilder.create<SubfieldOp>(
          clockModel.getLoc(),
          clockModel.getBodyBlock()->getArgument(*channelPort), "bits");
      auto clockFlag = clockBuilder.create<AsUIntPrimOp>(
          clockModel.getLoc(), clockBits.getResult());
      if (failed(goldengate::addFAMEClockEnable(
              clockModel, oldModelClock, clockFlag.getResult(), error)))
        return fail("FAME target clock enable: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME target clock enable produced invalid FIRRTL IR");
      llvm::SmallString<256> clockEnableIRPath(outputDir);
      llvm::sys::path::append(clockEnableIRPath,
                              "post-fame-clock-enable.mlir");
      std::error_code clockEnableWriteError;
      llvm::raw_fd_ostream clockEnableOut(clockEnableIRPath,
                                         clockEnableWriteError);
      if (clockEnableWriteError)
        return fail("cannot write FAME clock-enable MLIR: " +
                    clockEnableWriteError.message());
      module->print(clockEnableOut);
      clockEnableOut << '\n';
      clockEnableOut.close();
      llvm::outs() << "Added CIRCT FAME target clock enable in "
                   << clockEnableIRPath << '\n';

      if (failed(goldengate::addFAMEClockGate(
              circuit, clockModel, oldModelClock, error,
              clockBits.getResult())))
        return fail("FAME target clock gate: " + error);
      if (clockChannel->targetClocks.size() != 1)
        return fail("FAME hub clock XDC currently requires one analyzed clock");
      if (failed(goldengate::addFAMEClockConstraint(
              circuit, clockModel, oldModelClock,
              clockChannel->targetClocks.front(), error)))
        return fail("FAME hub clock XDC: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME target clock gate produced invalid FIRRTL IR");
      llvm::SmallString<256> clockGateIRPath(outputDir);
      llvm::sys::path::append(clockGateIRPath, "post-fame-clock-gate.mlir");
      std::error_code clockGateWriteError;
      llvm::raw_fd_ostream clockGateOut(clockGateIRPath,
                                       clockGateWriteError);
      if (clockGateWriteError)
        return fail("cannot write FAME clock-gate MLIR: " +
                    clockGateWriteError.message());
      module->print(clockGateOut);
      clockGateOut << '\n';
      clockGateOut.close();
      llvm::outs() << "Gated the CIRCT FAME target clock in "
                   << clockGateIRPath << '\n';

      // Rewrite the first six scalar sinks, the block-device response bundle,
      // the UART receive sink, the AXI ready sinks, and the AXI response
      // bundles, the TSI channels, and the trace trigger pipes. Resolve
      // targets after each mutation so port indices remain current.
      llvm::SmallVector<std::string> convertedInputs;
      llvm::SmallVector<std::string> convertedGlobalInputs;
      for (unsigned channelIndex = 0; channelIndex < 17; ++channelIndex) {
      auto dataHierarchy = goldengate::analyzeTopHierarchy(circuit, error);
      if (!dataHierarchy)
        return fail("FAME data hierarchy: " + error);
      auto dataAnnotations =
          circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
      std::optional<goldengate::GGChannelConnection> dataChannel;
      for (auto attr : dataAnnotations) {
        Annotation annotation(attr);
        if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
          continue;
        auto name = annotation.getMember<mlir::StringAttr>("globalName");
        if (!name || name.getValue() == clockChannel->name ||
            llvm::is_contained(convertedGlobalInputs, name.getValue().str()))
          continue;
        if (channelIndex == 6 && name.getValue() != "ep_bdev_resp_fwd")
          continue;
        if (channelIndex == 7 && name.getValue() != "ep_1_uart_rxd")
          continue;
        if (channelIndex == 8 && name.getValue() != "ep_2_axi4_ar_rev")
          continue;
        if (channelIndex == 9 && name.getValue() != "ep_2_axi4_aw_rev")
          continue;
        if (channelIndex == 10 && name.getValue() != "ep_2_axi4_w_rev")
          continue;
        if (channelIndex == 11 && name.getValue() != "ep_2_axi4_b_fwd")
          continue;
        if (channelIndex == 12 && name.getValue() != "ep_2_axi4_r_fwd")
          continue;
        if (channelIndex == 13 && name.getValue() != "ep_3_tsi_in_fwd")
          continue;
        if (channelIndex == 14 && name.getValue() != "ep_3_tsi_out_rev")
          continue;
        if (channelIndex == 15 && name.getValue() != "tracerv_triggerDebit")
          continue;
        if (channelIndex == 16 && name.getValue() != "tracerv_triggerCredit")
          continue;
        auto candidate = goldengate::analyzeChannelConnection(
            circuit, annotation, error);
        if (!candidate)
          return fail("FAME data channel: " + error);
        if (((channelIndex < 6 || (channelIndex >= 7 && channelIndex <= 10)) &&
             (candidate->kind == goldengate::ChannelKind::Pipe ||
              candidate->kind == goldengate::ChannelKind::DecoupledReverse) &&
             candidate->sinks.size() == 1) ||
            (channelIndex == 6 &&
             candidate->kind == goldengate::ChannelKind::DecoupledForward &&
             candidate->sinks.size() == 3) ||
            (channelIndex == 11 &&
             candidate->kind == goldengate::ChannelKind::DecoupledForward &&
             candidate->sinks.size() == 4) ||
            (channelIndex == 12 &&
             candidate->kind == goldengate::ChannelKind::DecoupledForward &&
             candidate->sinks.size() == 6) ||
            (channelIndex == 13 &&
             candidate->kind == goldengate::ChannelKind::DecoupledForward &&
             candidate->sinks.size() == 2) ||
            (channelIndex == 14 &&
             candidate->kind == goldengate::ChannelKind::DecoupledReverse &&
             candidate->sinks.size() == 1) ||
            (channelIndex >= 15 && channelIndex <= 16 &&
             candidate->kind == goldengate::ChannelKind::Pipe &&
             candidate->sinks.size() == 1)) {
          dataChannel = std::move(*candidate);
          break;
        }
      }
      if (!dataChannel)
        return fail("FAME block-device response input channel is missing");
      llvm::SmallVector<goldengate::ModelPortGroup> dataGroups;
      for (auto attr : dataAnnotations) {
        Annotation annotation(attr);
        if (!annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
          continue;
        auto group = goldengate::analyzeModelPortGroup(circuit, annotation,
                                                       error);
        if (!group)
          return fail("FAME data model ports: " + error);
        dataGroups.push_back(std::move(*group));
      }
      auto dataBindings = goldengate::bindChannelToModels(
          *dataChannel, *dataHierarchy, dataGroups, error);
      if (!dataBindings || dataBindings->size() != 1)
        return fail("FAME data model binding: " + error);
      auto dataGroup = *dataBindings->front().portGroup;
      if (dataGroup.ports.size() != dataChannel->sinks.size())
        return fail("FAME input model port count differs from its channel");
      llvm::SmallVector<goldengate::GGChannelConnection, 0> dataChannels;
      dataChannels.push_back(*dataChannel);
      llvm::SmallVector<FModuleLike> dataModels{dataGroup.module};
      auto dataPlan = goldengate::analyzeFAMEPorts(
          *dataHierarchy, *dataBindings, dataChannels, dataModels, error);
      if (!dataPlan || dataPlan->sinks.size() != 1)
        return fail("FAME data port plan: " + error);
      auto newTopData = dataPlan->sinks[0].portName;
      auto newModelData = dataGroup.name + "_sink";
      struct InputRename {
        std::string oldTop, newTop, oldModel, newModel;
      };
      llvm::SmallVector<InputRename> dataRenames;
      auto inputField = [](llvm::StringRef name, llvm::StringRef channel) {
        while (!name.empty() && !channel.empty() &&
               name.front() == channel.front()) {
          name = name.drop_front();
          channel = channel.drop_front();
        }
        return name;
      };
      for (unsigned modelPort : dataBindings->front().instancePorts) {
        std::optional<unsigned> topPort;
        for (const auto &connection : dataHierarchy->connections)
          if (connection.instance == dataBindings->front().instance &&
              connection.instancePort == modelPort) {
            if (topPort)
              return fail("FAME input has multiple top connections");
            topPort = connection.topPort;
          }
        if (!topPort)
          return fail("FAME input has no top connection");
        auto oldTop = dataHierarchy->top.getPortName(*topPort);
        auto oldModel = dataGroup.module.getPortName(modelPort);
        auto topField = dataBindings->front().instancePorts.size() == 1
                            ? std::string()
                            : ("." + inputField(oldTop, dataChannel->name).str());
        auto modelField = dataBindings->front().instancePorts.size() == 1
                              ? std::string()
                              : ("." + inputField(oldModel, dataGroup.name).str());
        dataRenames.push_back({
            prefix + dataHierarchy->top.getName().str() + ">" + oldTop.str(),
            prefix + dataHierarchy->top.getName().str() + ">" + newTopData +
                ".bits" + topField,
            prefix + dataGroup.module.getName().str() + ">" + oldModel.str(),
            prefix + dataGroup.module.getName().str() + ">" + newModelData +
                ".bits" + modelField});
      }
      if (failed(goldengate::rewriteFAMEInputChannel(
              *dataHierarchy, dataPlan->sinks[0], error)))
        return fail("FAME data input channel " + dataChannel->name + ": " +
                    error);
      llvm::SmallVector<mlir::Attribute> rewrittenDataAnnotations;
      unsigned dataTopRenames = 0, dataModelRenames = 0;
      unsigned dataReadySinkRenames = 0, dataValidSinkRenames = 0;
      for (auto attr : dataAnnotations) {
        Annotation annotation(attr);
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          for (const auto &rename : dataRenames)
            dataTopRenames += renameClock(annotation, "sinks", rename.oldTop,
                                          rename.newTop);
          if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
            mlir::NamedAttrList updatedInfo(info);
            for (const auto &rename : dataRenames) {
              if (auto readySink = info.getAs<mlir::StringAttr>("readySink");
                  readySink && readySink.getValue() == rename.oldTop) {
                updatedInfo.set("readySink",
                                mlir::StringAttr::get(&context, rename.newTop));
                ++dataReadySinkRenames;
              }
              if (auto validSink = info.getAs<mlir::StringAttr>("validSink");
                  validSink && validSink.getValue() == rename.oldTop) {
                updatedInfo.set("validSink",
                                mlir::StringAttr::get(&context, rename.newTop));
                ++dataValidSinkRenames;
              }
            }
            annotation.setMember("channelInfo", updatedInfo.getDictionary(&context));
          }
        }
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
          for (const auto &rename : dataRenames)
            dataModelRenames += renameClock(annotation, "ports",
                                            rename.oldModel, rename.newModel);
        rewrittenDataAnnotations.push_back(annotation.getAttr());
      }
      if (dataTopRenames != dataChannel->sinks.size() ||
          dataModelRenames != dataGroup.ports.size() ||
          dataReadySinkRenames !=
              (dataChannel->kind == goldengate::ChannelKind::DecoupledReverse) ||
          dataValidSinkRenames !=
              (dataChannel->kind == goldengate::ChannelKind::DecoupledForward))
        return fail("FAME data channel annotation targets were not unique");
      circuit->setAttr("rawAnnotations",
                       mlir::ArrayAttr::get(&context, rewrittenDataAnnotations));
      if (failed(mlir::verify(*module)))
        return fail("FAME data channel produced invalid FIRRTL IR");
      llvm::SmallString<256> dataIRPath(outputDir);
      llvm::SmallString<256> dataAnnotationPath(outputDir);
      llvm::StringRef ordinal = channelIndex == 0   ? "first"
                                : channelIndex == 1 ? "second"
                                : channelIndex == 2 ? "third"
                                : channelIndex == 3 ? "fourth"
                                : channelIndex == 4 ? "fifth"
                                : channelIndex == 5 ? "sixth"
                                : channelIndex == 6 ? "seventh"
                                : channelIndex == 7 ? "eighth"
                                : channelIndex == 8 ? "ninth"
                                : channelIndex == 9 ? "tenth"
                                : channelIndex == 10 ? "eleventh"
                                : channelIndex == 11 ? "twelfth"
                                : channelIndex == 12 ? "thirteenth"
                                : channelIndex == 13 ? "fourteenth"
                                : channelIndex == 14 ? "fifteenth"
                                : channelIndex == 15 ? "sixteenth"
                                                     : "seventeenth";
      llvm::sys::path::append(
          dataIRPath, "post-fame-" + ordinal + "-input-channel.mlir");
      llvm::sys::path::append(
          dataAnnotationPath, "post-fame-" + ordinal + "-input-channel.json");
      std::error_code dataWriteError;
      llvm::raw_fd_ostream dataOut(dataIRPath, dataWriteError);
      if (dataWriteError)
        return fail("cannot write FAME data-channel MLIR: " +
                    dataWriteError.message());
      module->print(dataOut);
      dataOut << '\n';
      dataOut.close();
      if (failed(goldengate::emitFAMEAnnotations(
              circuit, dataAnnotationPath, error)))
        return fail("cannot export FAME data annotations: " + error);
      llvm::outs() << "Channelized CIRCT FAME input " << dataChannel->name
                   << " in " << dataIRPath << '\n';

      // A clocked input begins fired so that reset cannot consume a token
      // before the first clock token establishes its target-cycle enable.
      auto dataModel = dyn_cast<FModuleOp>(dataGroup.module.getOperation());
      if (!dataModel || dataModel != clockModel)
        return fail("FAME input is not in the target clock model");
      llvm::SmallVector<goldengate::FAMEFiredChannel> thisInput{
          {dataGroup.name, true, clockFlag.getResult()}};
      if (failed(goldengate::ensureFAMEFiredRegisters(
              dataModel, thisInput, error)))
        return fail("FAME input fired register: " + error);
      llvm::SmallVector<std::string> thisInputNames{dataGroup.name};
      if (failed(goldengate::rewriteFAMEInputReadies(
              dataModel, thisInputNames, error)))
        return fail("FAME input ready: " + error);
      if (failed(goldengate::rewriteFAMEFiredStates(
              dataModel, thisInput, error)))
        return fail("FAME input fired state: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME input controls produced invalid FIRRTL IR");
      llvm::SmallString<256> dataControlIRPath(outputDir);
      llvm::sys::path::append(dataControlIRPath,
                              "post-fame-" + ordinal + "-input-control.mlir");
      std::error_code dataControlWriteError;
      llvm::raw_fd_ostream dataControlOut(dataControlIRPath,
                                          dataControlWriteError);
      if (dataControlWriteError)
        return fail("cannot write FAME input control MLIR: " +
                    dataControlWriteError.message());
      module->print(dataControlOut);
      dataControlOut << '\n';
      dataControlOut.close();
      llvm::outs() << "Added CIRCT FAME input controls for "
                   << dataChannel->name << " in " << dataControlIRPath << '\n';
      convertedInputs.push_back(dataGroup.name);
      convertedGlobalInputs.push_back(dataChannel->name);
      }

      // Retain the input-only cycle equation as a verified intermediate
      // boundary before any output is converted. The output must join this
      // equation when its fired and valid rules are installed.
      llvm::SmallVector<std::string> noOutputChannels;
      if (failed(goldengate::rewriteFAMEFinishing(
              clockModel, convertedInputs, noOutputChannels, clockGroup->name,
              error)))
        return fail("FAME partial cycle completion: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME partial cycle completion produced invalid FIRRTL IR");
      llvm::SmallString<256> finishingIRPath(outputDir);
      llvm::sys::path::append(finishingIRPath,
                              "post-fame-seventeen-input-finishing.mlir");
      std::error_code finishingWriteError;
      llvm::raw_fd_ostream finishingOut(finishingIRPath,
                                        finishingWriteError);
      if (finishingWriteError)
        return fail("cannot write partial FAME finishing MLIR: " +
                    finishingWriteError.message());
      module->print(finishingOut);
      finishingOut << '\n';
      finishingOut.close();
      llvm::outs() << "Wired partial CIRCT FAME cycle completion in "
                   << finishingIRPath << '\n';

      // The block-device forward halves have three and five payload fields.
      // The response ready, TSI ready, and model reset halves have one scalar
      // field each.
      // Resolve hierarchy after each port mutation.
      llvm::SmallVector<std::string> convertedOutputs;
      for (const auto &[outputName, outputOrdinal, expectedFields] :
           {std::tuple<llvm::StringRef, llvm::StringRef, unsigned>{
                "ep_bdev_data_fwd", "first", 3},
            {"ep_bdev_req_fwd", "second", 5},
            {"ep_bdev_resp_rev", "third", 1},
            {"ep_reset", "fourth", 1},
            {"ep_1_reset", "fifth", 1},
            {"ep_1_uart_txd", "sixth", 1},
            {"ep_2_reset", "seventh", 1},
            {"ep_3_reset", "eighth", 1},
            {"ep_3_tsi_in_rev", "ninth", 1},
            {"ep_3_tsi_out_fwd", "tenth", 2},
            {"ep_2_axi4_ar_fwd", "eleventh", 12},
            {"ep_2_axi4_w_fwd", "twelfth", 6},
            {"ep_2_axi4_aw_fwd", "thirteenth", 12},
            {"ep_2_axi4_r_rev", "fourteenth", 1},
            {"ep_2_axi4_b_rev", "fifteenth", 1},
            {"tracerv_tiletrace_reset", "sixteenth", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_valid", "seventeenth", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_iaddr", "eighteenth", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_insn", "nineteenth", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_priv", "twentieth", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_exception", "twenty-first", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_interrupt", "twenty-second", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_cause", "twenty-third", 1},
            {"tracerv_tiletrace_trace_retiredinsns_0_tval", "twenty-fourth", 1},
            {"tracerv_tiletrace_trace_time", "twenty-fifth", 1}}) {
      auto outputHierarchy = goldengate::analyzeTopHierarchy(circuit, error);
      if (!outputHierarchy)
        return fail("FAME output hierarchy: " + error);
      auto outputAnnotations =
          circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
      std::optional<goldengate::GGChannelConnection> outputChannel;
      llvm::SmallVector<goldengate::ModelPortGroup> outputGroups;
      for (auto attr : outputAnnotations) {
        Annotation annotation(attr);
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          auto name = annotation.getMember<mlir::StringAttr>("globalName");
          if (name && name.getValue() == outputName) {
            outputChannel = goldengate::analyzeChannelConnection(
                circuit, annotation, error);
            if (!outputChannel)
              return fail("FAME forward channel: " + error);
          }
        } else if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts)) {
          auto group = goldengate::analyzeModelPortGroup(circuit, annotation,
                                                        error);
          if (!group)
            return fail("FAME forward model ports: " + error);
          outputGroups.push_back(std::move(*group));
        }
      }
      auto expectedKind = outputName == "ep_bdev_resp_rev" ||
                                  outputName == "ep_3_tsi_in_rev" ||
                                  outputName == "ep_2_axi4_r_rev" ||
                                  outputName == "ep_2_axi4_b_rev"
                              ? goldengate::ChannelKind::DecoupledReverse
                              : outputName == "ep_reset" || outputName == "ep_1_reset" ||
                                        outputName == "ep_1_uart_txd" ||
                                        outputName == "ep_2_reset" ||
                                        outputName == "ep_3_reset" ||
                                        outputName == "tracerv_tiletrace_reset" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_valid" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_iaddr" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_insn" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_priv" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_exception" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_interrupt" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_cause" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_retiredinsns_0_tval" ||
                                        outputName ==
                                            "tracerv_tiletrace_trace_time"
                                    ? goldengate::ChannelKind::Pipe
                                    : goldengate::ChannelKind::DecoupledForward;
      if (!outputChannel || outputChannel->kind != expectedKind ||
          outputChannel->sources.size() != expectedFields)
        return fail("FAME block-device output payload count or kind differs from the oracle");
      auto outputBindings = goldengate::bindChannelToModels(
          *outputChannel, *outputHierarchy, outputGroups, error);
      if (!outputBindings || outputBindings->size() != 1)
        return fail("FAME forward model binding: " + error);
      auto outputGroup = *outputBindings->front().portGroup;
      llvm::SmallVector<goldengate::GGChannelConnection, 0> oneOutput{
          *outputChannel};
      llvm::SmallVector<FModuleLike> outputModels{outputGroup.module};
      auto outputPlan = goldengate::analyzeFAMEPorts(
          *outputHierarchy, *outputBindings, oneOutput, outputModels, error);
      if (!outputPlan || outputPlan->sources.size() != 1)
        return fail("FAME forward port plan: " + error);
      const auto &outputPort = outputPlan->sources.front();
      struct OutputRename {
        std::string oldTop, newTop, oldModel, newModel;
      };
      llvm::SmallVector<OutputRename> outputRenames;
      auto removeCommonPrefix = [](llvm::StringRef name, llvm::StringRef channel) {
        while (!name.empty() && !channel.empty() &&
               name.front() == channel.front()) {
          name = name.drop_front();
          channel = channel.drop_front();
        }
        return name;
      };
      for (unsigned modelPort : outputBindings->front().instancePorts) {
        std::optional<unsigned> topPort;
        for (const auto &connection : outputHierarchy->connections)
          if (connection.instance == outputBindings->front().instance &&
              connection.instancePort == modelPort) {
            if (topPort)
              return fail("FAME forward output has multiple top connections");
            topPort = connection.topPort;
          }
        if (!topPort)
          return fail("FAME forward output has no top connection");
        auto oldTop = outputHierarchy->top.getPortName(*topPort);
        auto oldModel = outputGroup.module.getPortName(modelPort);
        auto topField = outputBindings->front().instancePorts.size() == 1
                            ? std::string()
                            : ("." + removeCommonPrefix(oldTop, outputChannel->name).str());
        auto modelField = outputBindings->front().instancePorts.size() == 1
                              ? std::string()
                              : ("." + removeCommonPrefix(oldModel, outputGroup.name).str());
        outputRenames.push_back({
            prefix + outputHierarchy->top.getName().str() + ">" + oldTop.str(),
            prefix + outputHierarchy->top.getName().str() + ">" +
                outputPort.portName + ".bits" + topField,
            prefix + outputGroup.module.getName().str() + ">" + oldModel.str(),
            prefix + outputGroup.module.getName().str() + ">" +
                outputGroup.name + "_source.bits" + modelField});
      }
      if (failed(goldengate::rewriteFAMEOutputChannel(
              *outputHierarchy, outputPort, error)))
        return fail("FAME forward output channel: " + error);
      llvm::SmallVector<mlir::Attribute> rewrittenOutputAnnotations;
      unsigned outputTopRenames = 0, outputModelRenames = 0;
      unsigned outputValidRenames = 0, outputReadyRenames = 0;
      for (auto attr : outputAnnotations) {
        Annotation annotation(attr);
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          for (const auto &rename : outputRenames)
            outputTopRenames += renameClock(annotation, "sources",
                                            rename.oldTop, rename.newTop);
          if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
            mlir::NamedAttrList updated(info);
            for (const auto &rename : outputRenames)
              if (auto valid = info.getAs<mlir::StringAttr>("validSource");
                  valid && valid.getValue() == rename.oldTop) {
                updated.set("validSource",
                            mlir::StringAttr::get(&context, rename.newTop));
                ++outputValidRenames;
              }
            for (const auto &rename : outputRenames)
              if (auto ready = info.getAs<mlir::StringAttr>("readySource");
                  ready && ready.getValue() == rename.oldTop) {
                updated.set("readySource",
                            mlir::StringAttr::get(&context, rename.newTop));
                ++outputReadyRenames;
              }
            annotation.setMember("channelInfo", updated.getDictionary(&context));
          }
        }
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
          for (const auto &rename : outputRenames)
            outputModelRenames += renameClock(annotation, "ports",
                                              rename.oldModel, rename.newModel);
        rewrittenOutputAnnotations.push_back(annotation.getAttr());
      }
      if (outputTopRenames != expectedFields ||
          outputModelRenames != expectedFields ||
          outputValidRenames != (expectedKind == goldengate::ChannelKind::DecoupledForward) ||
          outputReadyRenames != (expectedKind == goldengate::ChannelKind::DecoupledReverse))
        return fail("FAME forward output annotation targets were not unique");
      circuit->setAttr("rawAnnotations", mlir::ArrayAttr::get(
          &context, rewrittenOutputAnnotations));
      if (failed(mlir::verify(*module)))
        return fail("FAME forward output produced invalid FIRRTL IR");
      llvm::SmallString<256> outputIRPath(outputDir), outputAnnotationPath(outputDir);
      llvm::sys::path::append(outputIRPath,
                              "post-fame-" + outputOrdinal + "-output-channel.mlir");
      llvm::sys::path::append(outputAnnotationPath,
                              "post-fame-" + outputOrdinal + "-output-channel.json");
      std::error_code outputWriteError;
      llvm::raw_fd_ostream outputOut(outputIRPath, outputWriteError);
      if (outputWriteError)
        return fail("cannot write FAME forward output MLIR: " +
                    outputWriteError.message());
      module->print(outputOut);
      outputOut << '\n';
      outputOut.close();
      if (failed(goldengate::emitFAMEAnnotations(
              circuit, outputAnnotationPath, error)))
        return fail("cannot export FAME forward output annotations: " + error);
      llvm::outs() << "Channelized CIRCT FAME output " << outputChannel->name
                   << " in " << outputIRPath << '\n';

      llvm::SmallVector<goldengate::FAMEFiredChannel> thisOutputFired{
          {outputGroup.name, false, {}}};
      mlir::Value outputClockEnable;
      clockModel.walk([&](RegResetOp op) {
        if (op.getName() == oldModelClock + "_enabled")
          outputClockEnable = op.getResult();
      });
      if (!outputClockEnable)
        return fail("FAME output clock enable is missing");
      thisOutputFired.front().clockDomainEnable = outputClockEnable;
      if (failed(goldengate::ensureFAMEFiredRegisters(
              clockModel, thisOutputFired, error)))
        return fail("FAME output fired register: " + error);
      llvm::SmallVector<goldengate::LocalChannelDependency> thisOutputDeps{
          outputDependencies.at(outputGroup.name)};
      if (failed(goldengate::rewriteFAMEOutputValids(
              clockModel, thisOutputDeps, error)))
        return fail("FAME output valid: " + error);
      if (failed(goldengate::rewriteFAMEFiredStates(
              clockModel, thisOutputFired, error)))
        return fail("FAME output fired state: " + error);
      convertedOutputs.push_back(outputGroup.name);
      if (failed(goldengate::rewriteFAMEFinishing(
              clockModel, convertedInputs, convertedOutputs, clockGroup->name,
              error)))
        return fail("FAME output cycle completion: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME output controls produced invalid FIRRTL IR");
      llvm::SmallString<256> outputControlIRPath(outputDir);
      llvm::sys::path::append(outputControlIRPath,
                              "post-fame-" + outputOrdinal + "-output-control.mlir");
      std::error_code outputControlWriteError;
      llvm::raw_fd_ostream outputControlOut(outputControlIRPath,
                                            outputControlWriteError);
      if (outputControlWriteError)
        return fail("cannot write FAME output controls: " +
                    outputControlWriteError.message());
      module->print(outputControlOut);
      outputControlOut << '\n';
      outputControlOut.close();
      llvm::outs() << "Added CIRCT FAME output controls for "
                   << outputChannel->name << " in " << outputControlIRPath << '\n';
      }
      // Create scalar boundary queues from the post-FAME channel annotations.
      // Equal (payload width, latency) pairs share a module definition.
      if (failed(goldengate::addFAMEBoundaryPipeChannels(circuit, error)))
        return fail("FAME simulator PipeChannel: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME simulator PipeChannel produced invalid FIRRTL IR");
      llvm::SmallString<256> pipeIRPath(outputDir);
      llvm::sys::path::append(pipeIRPath, "post-fame-first-pipe.mlir");
      std::error_code pipeWriteError;
      llvm::raw_fd_ostream pipeOut(pipeIRPath, pipeWriteError);
      if (pipeWriteError)
        return fail("cannot write FAME simulator PipeChannel: " +
                    pipeWriteError.message());
      module->print(pipeOut);
      pipeOut << '\n';
      pipeOut.close();
      llvm::outs() << "Materialized CIRCT FAME boundary PipeChannels in "
                   << pipeIRPath << '\n';
      if (failed(goldengate::addFAMEPipeWrapper(circuit, error)))
        return fail("FAME PipeChannel simulator wrapper: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME PipeChannel wrapper produced invalid FIRRTL IR");
      llvm::SmallString<256> wrapperIRPath(outputDir);
      llvm::sys::path::append(wrapperIRPath,
                              "post-fame-first-pipe-wrapper.mlir");
      std::error_code wrapperWriteError;
      llvm::raw_fd_ostream wrapperOut(wrapperIRPath, wrapperWriteError);
      if (wrapperWriteError)
        return fail("cannot write FAME PipeChannel wrapper: " +
                    wrapperWriteError.message());
      module->print(wrapperOut);
      wrapperOut << '\n';
      wrapperOut.close();
      llvm::outs() << "Connected CIRCT FAME boundary PipeChannels in "
                   << wrapperIRPath << '\n';
      if (failed(goldengate::addFAMEBoundaryReadyValidChannels(circuit, error)))
        return fail("FAME ReadyValidChannel: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME ReadyValidChannel produced invalid FIRRTL IR");
      llvm::SmallString<256> readyValidIRPath(outputDir);
      llvm::sys::path::append(readyValidIRPath, "post-fame-ready-valid-wrapper.mlir");
      std::error_code readyValidWriteError;
      llvm::raw_fd_ostream readyValidOut(readyValidIRPath, readyValidWriteError);
      if (readyValidWriteError)
        return fail("cannot write ReadyValidChannel wrapper: " + readyValidWriteError.message());
      module->print(readyValidOut);
      readyValidOut << '\n';
      readyValidOut.close();
      llvm::outs() << "Connected CIRCT FAME boundary ReadyValidChannels in "
                   << readyValidIRPath << '\n';
      if (failed(goldengate::addFAMEClockChannel(circuit, error)))
        return fail("FAME clock channel: " + error);
      if (failed(mlir::verify(*module)))
        return fail("FAME clock channel produced invalid FIRRTL IR");
      llvm::SmallString<256> clockWrapperIRPath(outputDir);
      llvm::sys::path::append(clockWrapperIRPath, "post-fame-clock-wrapper.mlir");
      std::error_code clockWrapperWriteError;
      llvm::raw_fd_ostream clockWrapperOut(clockWrapperIRPath, clockWrapperWriteError);
      if (clockWrapperWriteError)
        return fail("cannot write clock channel wrapper: " + clockWrapperWriteError.message());
      module->print(clockWrapperOut);
      clockWrapperOut << '\n';
      clockWrapperOut.close();
      llvm::outs() << "Connected CIRCT FAME Boolean clock tokens in "
                   << clockWrapperIRPath << '\n';
      if (failed(goldengate::activateFAMEPipeWrapper(circuit, error)))
        return fail("FAME PipeChannel wrapper activation: " + error);
      if (failed(mlir::verify(*module)))
        return fail("active FAME PipeChannel wrapper produced invalid FIRRTL IR");
      llvm::SmallString<256> activeIRPath(outputDir), activeAnnotationPath(outputDir);
      llvm::sys::path::append(activeIRPath,
                              "post-fame-first-pipe-active-wrapper.mlir");
      llvm::sys::path::append(activeAnnotationPath,
                              "post-fame-first-pipe-active-wrapper-all.json");
      std::error_code activeWriteError;
      llvm::raw_fd_ostream activeOut(activeIRPath, activeWriteError);
      if (activeWriteError)
        return fail("cannot write active FAME PipeChannel wrapper: " +
                    activeWriteError.message());
      module->print(activeOut);
      activeOut << '\n';
      activeOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, activeAnnotationPath,
                                                error)))
        return fail("cannot write active FAME wrapper annotations: " + error);
      llvm::outs() << "Activated CIRCT FAME PipeChannel wrapper in "
                   << activeIRPath << '\n';
      if (failed(goldengate::addSingleClockBridge(circuit, error)))
        return fail("single ClockBridge mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("single ClockBridge mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> clockBridgeIRPath(outputDir), clockBridgeAnnotationPath(outputDir);
      llvm::sys::path::append(clockBridgeIRPath, "post-fame-clock-bridge.mlir");
      llvm::sys::path::append(clockBridgeAnnotationPath, "post-fame-clock-bridge-all.json");
      std::error_code clockBridgeWriteError;
      llvm::raw_fd_ostream clockBridgeOut(clockBridgeIRPath, clockBridgeWriteError);
      if (clockBridgeWriteError)
        return fail("cannot write ClockBridge mapping: " + clockBridgeWriteError.message());
      module->print(clockBridgeOut);
      clockBridgeOut << '\n';
      clockBridgeOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, clockBridgeAnnotationPath, error)))
        return fail("cannot write ClockBridge annotations: " + error);
      llvm::outs() << "Mapped CIRCT single-clock ClockBridge token producer in "
                   << clockBridgeIRPath << '\n';
      // Current baseline is the recorded U250 CtrlNastiKey(32,25,12).
      // Platform configuration and the widget crossbar are subsequent ports.
      if (failed(goldengate::mapClockBridgeControl(circuit, 25, 12, error)))
        return fail("ClockBridge MCRFile mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("ClockBridge MCRFile mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> controlIRPath(outputDir), controlAnnotationPath(outputDir);
      llvm::sys::path::append(controlIRPath, "post-fame-clock-bridge-control.mlir");
      llvm::sys::path::append(controlAnnotationPath, "post-fame-clock-bridge-control-all.json");
      std::error_code controlWriteError;
      llvm::raw_fd_ostream controlOut(controlIRPath, controlWriteError);
      if (controlWriteError)
        return fail("cannot write ClockBridge control mapping: " + controlWriteError.message());
      module->print(controlOut);
      controlOut << '\n';
      controlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlAnnotationPath, error)))
        return fail("cannot write ClockBridge control annotations: " + error);
      llvm::outs() << "Mapped CIRCT ClockBridge MCRFile transactions in " << controlIRPath << '\n';
      if (failed(goldengate::addResetPulseBridge(circuit, error)))
        return fail("ResetPulseBridge mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("ResetPulseBridge mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> resetIRPath(outputDir), resetAnnotationPath(outputDir);
      llvm::sys::path::append(resetIRPath, "post-fame-reset-pulse-bridge.mlir");
      llvm::sys::path::append(resetAnnotationPath, "post-fame-reset-pulse-bridge-all.json");
      std::error_code resetWriteError;
      llvm::raw_fd_ostream resetOut(resetIRPath, resetWriteError);
      if (resetWriteError)
        return fail("cannot write ResetPulseBridge mapping: " + resetWriteError.message());
      module->print(resetOut);
      resetOut << '\n';
      resetOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, resetAnnotationPath, error)))
        return fail("cannot write ResetPulseBridge annotations: " + error);
      llvm::outs() << "Mapped CIRCT ResetPulseBridge token producer in " << resetIRPath << '\n';
      if (failed(goldengate::mapResetPulseBridgeControl(circuit, 25, 12, error)))
        return fail("ResetPulseBridge MCRFile mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("ResetPulseBridge MCRFile mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> resetControlIRPath(outputDir), resetControlAnnotationPath(outputDir);
      llvm::sys::path::append(resetControlIRPath, "post-fame-reset-pulse-bridge-control.mlir");
      llvm::sys::path::append(resetControlAnnotationPath, "post-fame-reset-pulse-bridge-control-all.json");
      std::error_code resetControlWriteError;
      llvm::raw_fd_ostream resetControlOut(resetControlIRPath, resetControlWriteError);
      if (resetControlWriteError)
        return fail("cannot write ResetPulseBridge control mapping: " + resetControlWriteError.message());
      module->print(resetControlOut);
      resetControlOut << '\n';
      resetControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, resetControlAnnotationPath, error)))
        return fail("cannot write ResetPulseBridge control annotations: " + error);
      llvm::outs() << "Mapped CIRCT ResetPulseBridge MCRFile transactions in "
                   << resetControlIRPath << '\n';
      if (failed(goldengate::addUARTSerialEngine(circuit, error)))
        return fail("UART serial mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("UART serial mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> uartIRPath(outputDir), uartAnnotationPath(outputDir);
      llvm::sys::path::append(uartIRPath, "post-fame-uart-serial.mlir");
      llvm::sys::path::append(uartAnnotationPath, "post-fame-uart-serial-all.json");
      std::error_code uartWriteError;
      llvm::raw_fd_ostream uartOut(uartIRPath, uartWriteError);
      if (uartWriteError)
        return fail("cannot write UART serial mapping: " + uartWriteError.message());
      module->print(uartOut);
      uartOut << '\n';
      uartOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, uartAnnotationPath, error)))
        return fail("cannot write UART serial annotations: " + error);
      llvm::outs() << "Mapped CIRCT UART serial engine in " << uartIRPath << '\n';
      if (failed(goldengate::addUARTByteQueues(circuit, error)))
        return fail("UART byte queue mapping: " + error);
      if (failed(mlir::verify(*module)))
        return fail("UART byte queue mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> uartQueuesIRPath(outputDir), uartQueuesAnnotationPath(outputDir);
      llvm::sys::path::append(uartQueuesIRPath, "post-fame-uart-byte-queues.mlir");
      llvm::sys::path::append(uartQueuesAnnotationPath, "post-fame-uart-byte-queues-all.json");
      std::error_code uartQueuesWriteError;
      llvm::raw_fd_ostream uartQueuesOut(uartQueuesIRPath, uartQueuesWriteError);
      if (uartQueuesWriteError)
        return fail("cannot write UART byte queues: " + uartQueuesWriteError.message());
      module->print(uartQueuesOut);
      uartQueuesOut << '\n';
      uartQueuesOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, uartQueuesAnnotationPath, error)))
        return fail("cannot write UART byte queue annotations: " + error);
      llvm::outs() << "Mapped CIRCT UART byte queues in " << uartQueuesIRPath << '\n';
      if (failed(goldengate::addUARTMMIOBank(circuit, error)))
        return fail("UART MMIO bank: " + error);
      if (failed(mlir::verify(*module)))
        return fail("UART MMIO bank produced invalid FIRRTL IR");
      llvm::SmallString<256> uartMMIOIRPath(outputDir), uartMMIOAnnotationPath(outputDir);
      llvm::sys::path::append(uartMMIOIRPath, "post-fame-uart-mmio.mlir");
      llvm::sys::path::append(uartMMIOAnnotationPath, "post-fame-uart-mmio-all.json");
      std::error_code uartMMIOWriteError;
      llvm::raw_fd_ostream uartMMIOOut(uartMMIOIRPath, uartMMIOWriteError);
      if (uartMMIOWriteError)
        return fail("cannot write UART MMIO bank: " + uartMMIOWriteError.message());
      module->print(uartMMIOOut);
      uartMMIOOut << '\n';
      uartMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, uartMMIOAnnotationPath, error)))
        return fail("cannot write UART MMIO bank annotations: " + error);
      llvm::outs() << "Mapped CIRCT UART MMIO bank in " << uartMMIOIRPath << '\n';
      if (failed(goldengate::mapUARTBridgeControl(circuit, 25, 12, error)))
        return fail("UART MCRFile transactions: " + error);
      if (failed(mlir::verify(*module)))
        return fail("UART MCRFile transactions produced invalid FIRRTL IR");
      llvm::SmallString<256> uartControlIRPath(outputDir), uartControlAnnotationPath(outputDir);
      llvm::sys::path::append(uartControlIRPath, "post-fame-uart-control.mlir");
      llvm::sys::path::append(uartControlAnnotationPath, "post-fame-uart-control-all.json");
      std::error_code uartControlWriteError;
      llvm::raw_fd_ostream uartControlOut(uartControlIRPath, uartControlWriteError);
      if (uartControlWriteError)
        return fail("cannot write UART MCRFile transactions: " + uartControlWriteError.message());
      module->print(uartControlOut);
      uartControlOut << '\n';
      uartControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, uartControlAnnotationPath, error)))
        return fail("cannot write UART MCRFile transactions annotations: " + error);
      llvm::outs() << "Mapped CIRCT UART MCRFile transactions in " << uartControlIRPath << '\n';
      if (failed(goldengate::addPeekPokeCycleEngine(circuit, error)))
        return fail("PeekPoke cycle engine: " + error);
      if (failed(mlir::verify(*module)))
        return fail("PeekPoke cycle engine produced invalid FIRRTL IR");
      llvm::SmallString<256> peekPokeIRPath(outputDir);
      llvm::sys::path::append(peekPokeIRPath, "post-fame-peek-poke-cycle.mlir");
      std::error_code peekPokeWriteError;
      llvm::raw_fd_ostream peekPokeOut(peekPokeIRPath, peekPokeWriteError);
      if (peekPokeWriteError)
        return fail("cannot write PeekPoke cycle engine: " + peekPokeWriteError.message());
      module->print(peekPokeOut); peekPokeOut << '\n'; peekPokeOut.close();
      llvm::SmallString<256> peekPokeAnnotationPath(outputDir);
      llvm::sys::path::append(peekPokeAnnotationPath, "post-fame-peek-poke-cycle-all.json");
      if (failed(goldengate::emitAllAnnotations(circuit, peekPokeAnnotationPath, error)))
        return fail("PeekPoke annotations: " + error);
      llvm::outs() << "Mapped CIRCT PeekPoke cycle scheduling in " << peekPokeIRPath << '\n';
      if (failed(goldengate::addPeekPokeMMIOBank(circuit, error)))
        return fail("PeekPoke STEP queue and MMIO bank: " + error);
      if (failed(mlir::verify(*module)))
        return fail("PeekPoke MMIO produced invalid FIRRTL IR");
      llvm::SmallString<256> peekPokeMMIOPath(outputDir), peekPokeMMIOAnnotations(outputDir);
      llvm::sys::path::append(peekPokeMMIOPath, "post-fame-peek-poke-mmio.mlir");
      llvm::sys::path::append(peekPokeMMIOAnnotations, "post-fame-peek-poke-mmio-all.json");
      std::error_code peekPokeMMIOError;
      llvm::raw_fd_ostream peekPokeMMIOOut(peekPokeMMIOPath, peekPokeMMIOError);
      if (peekPokeMMIOError)
        return fail("cannot write PeekPoke MMIO: " + peekPokeMMIOError.message());
      module->print(peekPokeMMIOOut); peekPokeMMIOOut << '\n'; peekPokeMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, peekPokeMMIOAnnotations, error)))
        return fail("PeekPoke MMIO annotations: " + error);
      llvm::outs() << "Mapped CIRCT PeekPoke STEP queue and MMIO bank in " << peekPokeMMIOPath << '\n';
      if (failed(goldengate::mapPeekPokeBridgeControl(circuit, 25, 12, error)))
        return fail("PeekPoke MCRFile transactions: " + error);
      if (failed(mlir::verify(*module)))
        return fail("PeekPoke MCRFile transactions produced invalid FIRRTL IR");
      llvm::SmallString<256> peekPokeControlPath(outputDir), peekPokeControlAnnotations(outputDir);
      llvm::sys::path::append(peekPokeControlPath, "post-fame-peek-poke-control.mlir");
      llvm::sys::path::append(peekPokeControlAnnotations, "post-fame-peek-poke-control-all.json");
      std::error_code peekPokeControlError;
      llvm::raw_fd_ostream peekPokeControlOut(peekPokeControlPath, peekPokeControlError);
      if (peekPokeControlError)
        return fail("cannot write PeekPoke MCRFile transactions: " + peekPokeControlError.message());
      module->print(peekPokeControlOut); peekPokeControlOut << '\n'; peekPokeControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, peekPokeControlAnnotations, error)))
        return fail("PeekPoke MCRFile transactions annotations: " + error);
      llvm::outs() << "Mapped CIRCT PeekPoke MCRFile transactions in " << peekPokeControlPath << '\n';
      if (failed(goldengate::addTracerVTokenEngine(circuit, error)))
        return fail("TracerV token engine: " + error);
      if (failed(mlir::verify(*module)))
        return fail("TracerV token engine produced invalid FIRRTL IR");
      llvm::SmallString<256> tracerTokenPath(outputDir), tracerTokenAnnotations(outputDir);
      llvm::sys::path::append(tracerTokenPath, "post-fame-tracerv-token.mlir");
      llvm::sys::path::append(tracerTokenAnnotations, "post-fame-tracerv-token-all.json");
      std::error_code tracerTokenError;
      llvm::raw_fd_ostream tracerTokenOut(tracerTokenPath, tracerTokenError);
      if (tracerTokenError)
        return fail("cannot write TracerV token engine: " + tracerTokenError.message());
      module->print(tracerTokenOut); tracerTokenOut << '\n'; tracerTokenOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tracerTokenAnnotations, error)))
        return fail("TracerV token engine annotations: " + error);
      llvm::outs() << "Mapped CIRCT TracerV token acceptance and stream packing in " << tracerTokenPath << '\n';
      if (failed(goldengate::addTracerVTriggerConfig(circuit, error)))
        return fail("TracerV trigger configuration: " + error);
      if (failed(mlir::verify(*module)))
        return fail("TracerV trigger configuration produced invalid FIRRTL IR");
      llvm::SmallString<256> tracerConfigPath(outputDir), tracerConfigAnnotations(outputDir);
      llvm::sys::path::append(tracerConfigPath, "post-fame-tracerv-trigger.mlir");
      llvm::sys::path::append(tracerConfigAnnotations, "post-fame-tracerv-trigger-all.json");
      std::error_code tracerConfigError;
      llvm::raw_fd_ostream tracerConfigOut(tracerConfigPath, tracerConfigError);
      if (tracerConfigError)
        return fail("cannot write TracerV trigger configuration: " + tracerConfigError.message());
      module->print(tracerConfigOut); tracerConfigOut << '\n'; tracerConfigOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tracerConfigAnnotations, error)))
        return fail("TracerV trigger configuration annotations: " + error);
      llvm::outs() << "Mapped CIRCT TracerV trigger configuration in " << tracerConfigPath << '\n';
      if (failed(goldengate::mapTracerVBridgeControl(circuit, 25, 12, error)))
        return fail("TracerV MCRFile transactions: " + error);
      if (failed(mlir::verify(*module)))
        return fail("TracerV MCRFile transactions produced invalid FIRRTL IR");
      llvm::SmallString<256> tracerControlPath(outputDir), tracerControlAnnotations(outputDir);
      llvm::sys::path::append(tracerControlPath, "post-fame-tracerv-control.mlir");
      llvm::sys::path::append(tracerControlAnnotations, "post-fame-tracerv-control-all.json");
      std::error_code tracerControlError;
      llvm::raw_fd_ostream tracerControlOut(tracerControlPath, tracerControlError);
      if (tracerControlError)
        return fail("cannot write TracerV MCRFile transactions: " + tracerControlError.message());
      module->print(tracerControlOut); tracerControlOut << '\n'; tracerControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tracerControlAnnotations, error)))
        return fail("TracerV MCRFile transaction annotations: " + error);
      llvm::outs() << "Mapped CIRCT TracerV MCRFile transactions in " << tracerControlPath << '\n';
      if (failed(goldengate::addTracerVStreamQueue(circuit, error)))
        return fail("TracerV stream queue: " + error);
      if (failed(mlir::verify(*module)))
        return fail("TracerV stream queue produced invalid FIRRTL IR");
      llvm::SmallString<256> tracerQueuePath(outputDir), tracerQueueAnnotations(outputDir);
      llvm::sys::path::append(tracerQueuePath, "post-fame-tracerv-stream-queue.mlir");
      llvm::sys::path::append(tracerQueueAnnotations, "post-fame-tracerv-stream-queue-all.json");
      std::error_code tracerQueueError;
      llvm::raw_fd_ostream tracerQueueOut(tracerQueuePath, tracerQueueError);
      if (tracerQueueError)
        return fail("cannot write TracerV stream queue: " + tracerQueueError.message());
      module->print(tracerQueueOut); tracerQueueOut << '\n'; tracerQueueOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tracerQueueAnnotations, error)))
        return fail("TracerV stream queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT TracerV CPU stream in " << tracerQueuePath << '\n';
      if (failed(goldengate::addCPUStreamRead(circuit, error)))
        return fail("CPU stream read transport: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream read transport produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuReadPath(outputDir), cpuReadAnnotations(outputDir);
      llvm::sys::path::append(cpuReadPath, "post-fame-cpu-stream-read.mlir");
      llvm::sys::path::append(cpuReadAnnotations, "post-fame-cpu-stream-read-all.json");
      std::error_code cpuReadError;
      llvm::raw_fd_ostream cpuReadOut(cpuReadPath, cpuReadError);
      if (cpuReadError)
        return fail("cannot write CPU stream read transport: " + cpuReadError.message());
      module->print(cpuReadOut); cpuReadOut << '\n'; cpuReadOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuReadAnnotations, error)))
        return fail("CPU stream read transport annotations: " + error);
      llvm::outs() << "Mapped CIRCT CPU stream read transport in " << cpuReadPath << '\n';
      if (failed(goldengate::addCPUStreamCountBank(circuit, error)))
        return fail("CPU stream count bank: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream count bank produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuCountPath(outputDir), cpuCountAnnotations(outputDir);
      llvm::sys::path::append(cpuCountPath, "post-fame-cpu-stream-count.mlir");
      llvm::sys::path::append(cpuCountAnnotations, "post-fame-cpu-stream-count-all.json");
      std::error_code cpuCountError;
      llvm::raw_fd_ostream cpuCountOut(cpuCountPath, cpuCountError);
      if (cpuCountError)
        return fail("cannot write CPU stream count bank: " + cpuCountError.message());
      module->print(cpuCountOut); cpuCountOut << '\n'; cpuCountOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuCountAnnotations, error)))
        return fail("CPU stream count bank annotations: " + error);
      llvm::outs() << "Mapped CIRCT CPU stream count MMIO bank in " << cpuCountPath << '\n';
      if (failed(goldengate::mapCPUStreamControl(circuit, 25, 12, error)))
        return fail("CPU stream MCRFile transactions: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream MCRFile produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuControlPath(outputDir), cpuControlAnnotations(outputDir);
      llvm::sys::path::append(cpuControlPath, "post-fame-cpu-stream-control.mlir");
      llvm::sys::path::append(cpuControlAnnotations, "post-fame-cpu-stream-control-all.json");
      std::error_code cpuControlError;
      llvm::raw_fd_ostream cpuControlOut(cpuControlPath, cpuControlError);
      if (cpuControlError)
        return fail("cannot write CPU stream MCRFile: " + cpuControlError.message());
      module->print(cpuControlOut); cpuControlOut << '\n'; cpuControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuControlAnnotations, error)))
        return fail("CPU stream MCRFile annotations: " + error);
      llvm::outs() << "Mapped CIRCT CPU stream MCRFile transactions in " << cpuControlPath << '\n';
      if (failed(goldengate::addEmptyCPUStreamWrite(circuit, error)))
        return fail("empty CPU stream write boundary: " + error);
      if (failed(mlir::verify(*module)))
        return fail("empty CPU stream write produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuWritePath(outputDir), cpuWriteAnnotations(outputDir);
      llvm::sys::path::append(cpuWritePath, "post-fame-cpu-stream-write.mlir");
      llvm::sys::path::append(cpuWriteAnnotations, "post-fame-cpu-stream-write-all.json");
      std::error_code cpuWriteError;
      llvm::raw_fd_ostream cpuWriteOut(cpuWritePath, cpuWriteError);
      if (cpuWriteError)
        return fail("cannot write empty CPU stream write boundary: " + cpuWriteError.message());
      module->print(cpuWriteOut); cpuWriteOut << '\n'; cpuWriteOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuWriteAnnotations, error)))
        return fail("empty CPU stream write annotations: " + error);
      llvm::outs() << "Mapped CIRCT empty CPU stream write boundary in " << cpuWritePath << '\n';
      if (failed(goldengate::addCPUStreamWriteBuffer(circuit, error)))
        return fail("CPU stream AW/W buffer: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream AW/W buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuWriteBufferPath(outputDir), cpuWriteBufferAnnotations(outputDir);
      llvm::sys::path::append(cpuWriteBufferPath, "post-fame-cpu-stream-write-buffer.mlir");
      llvm::sys::path::append(cpuWriteBufferAnnotations, "post-fame-cpu-stream-write-buffer-all.json");
      std::error_code cpuWriteBufferError;
      llvm::raw_fd_ostream cpuWriteBufferOut(cpuWriteBufferPath, cpuWriteBufferError);
      if (cpuWriteBufferError)
        return fail("cannot write CPU stream AW/W buffer: " + cpuWriteBufferError.message());
      module->print(cpuWriteBufferOut); cpuWriteBufferOut << '\n'; cpuWriteBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuWriteBufferAnnotations, error)))
        return fail("CPU stream AW/W buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT CPU stream AW/W requests in " << cpuWriteBufferPath << '\n';
      if (failed(goldengate::addCPUStreamReadBuffer(circuit, error)))
        return fail("CPU stream AR buffer: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream AR buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuReadBufferPath(outputDir), cpuReadBufferAnnotations(outputDir);
      llvm::sys::path::append(cpuReadBufferPath, "post-fame-cpu-stream-read-buffer.mlir");
      llvm::sys::path::append(cpuReadBufferAnnotations, "post-fame-cpu-stream-read-buffer-all.json");
      std::error_code cpuReadBufferError;
      llvm::raw_fd_ostream cpuReadBufferOut(cpuReadBufferPath, cpuReadBufferError);
      if (cpuReadBufferError)
        return fail("cannot write CPU stream AR buffer: " + cpuReadBufferError.message());
      module->print(cpuReadBufferOut); cpuReadBufferOut << '\n'; cpuReadBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuReadBufferAnnotations, error)))
        return fail("CPU stream AR buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT CPU stream AR requests in " << cpuReadBufferPath << '\n';
      if (failed(goldengate::addCPUStreamResponseBuffer(circuit, error)))
        return fail("CPU stream R response buffer: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream R response buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuResponseBufferPath(outputDir), cpuResponseBufferAnnotations(outputDir);
      llvm::sys::path::append(cpuResponseBufferPath, "post-fame-cpu-stream-response-buffer.mlir");
      llvm::sys::path::append(cpuResponseBufferAnnotations, "post-fame-cpu-stream-response-buffer-all.json");
      std::error_code cpuResponseBufferError;
      llvm::raw_fd_ostream cpuResponseBufferOut(cpuResponseBufferPath, cpuResponseBufferError);
      if (cpuResponseBufferError)
        return fail("cannot write CPU stream R response buffer: " + cpuResponseBufferError.message());
      module->print(cpuResponseBufferOut); cpuResponseBufferOut << '\n'; cpuResponseBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuResponseBufferAnnotations, error)))
        return fail("CPU stream R response buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT CPU stream R response beats in " << cpuResponseBufferPath << '\n';
      if (failed(goldengate::addCPUStreamWriteResponseBuffer(circuit, error)))
        return fail("CPU stream B response buffer: " + error);
      if (failed(mlir::verify(*module)))
        return fail("CPU stream B response buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuWriteResponseBufferPath(outputDir), cpuWriteResponseBufferAnnotations(outputDir);
      llvm::sys::path::append(cpuWriteResponseBufferPath, "post-fame-cpu-stream-write-response-buffer.mlir");
      llvm::sys::path::append(cpuWriteResponseBufferAnnotations, "post-fame-cpu-stream-write-response-buffer-all.json");
      std::error_code cpuWriteResponseBufferError;
      llvm::raw_fd_ostream cpuWriteResponseBufferOut(cpuWriteResponseBufferPath, cpuWriteResponseBufferError);
      if (cpuWriteResponseBufferError)
        return fail("cannot write CPU stream B response buffer: " + cpuWriteResponseBufferError.message());
      module->print(cpuWriteResponseBufferOut); cpuWriteResponseBufferOut << '\n'; cpuWriteResponseBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuWriteResponseBufferAnnotations, error)))
        return fail("CPU stream B response buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT CPU stream B responses in " << cpuWriteResponseBufferPath << '\n';
      if (failed(goldengate::addLoadMemWriter(circuit, error)))
        return fail("LoadMem writer: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem writer produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemWriterPath(outputDir), loadMemWriterAnnotations(outputDir);
      llvm::sys::path::append(loadMemWriterPath, "post-fame-loadmem-writer.mlir");
      llvm::sys::path::append(loadMemWriterAnnotations, "post-fame-loadmem-writer-all.json");
      std::error_code loadMemWriterError;
      llvm::raw_fd_ostream loadMemWriterOut(loadMemWriterPath, loadMemWriterError);
      if (loadMemWriterError)
        return fail("cannot write LoadMem writer: " + loadMemWriterError.message());
      module->print(loadMemWriterOut); loadMemWriterOut << '\n'; loadMemWriterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemWriterAnnotations, error)))
        return fail("LoadMem writer annotations: " + error);
      llvm::outs() << "Mapped CIRCT LoadMem writer bursts in " << loadMemWriterPath << '\n';
      if (failed(goldengate::addLoadMemRequests(circuit, error)))
        return fail("LoadMem requests: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem requests produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemRequestsPath(outputDir), loadMemRequestsAnnotations(outputDir);
      llvm::sys::path::append(loadMemRequestsPath, "post-fame-loadmem-requests.mlir");
      llvm::sys::path::append(loadMemRequestsAnnotations, "post-fame-loadmem-requests-all.json");
      std::error_code loadMemRequestsError;
      llvm::raw_fd_ostream loadMemRequestsOut(loadMemRequestsPath, loadMemRequestsError);
      if (loadMemRequestsError)
        return fail("cannot write LoadMem requests: " + loadMemRequestsError.message());
      module->print(loadMemRequestsOut); loadMemRequestsOut << '\n'; loadMemRequestsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemRequestsAnnotations, error)))
        return fail("LoadMem requests annotations: " + error);
      llvm::outs() << "Queued CIRCT LoadMem writes and zero-fill requests in " << loadMemRequestsPath << '\n';
      if (failed(goldengate::addLoadMemWriteMMIO(circuit, error)))
        return fail("LoadMem write MMIO: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem write MMIO produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemWriteMMIOPath(outputDir), loadMemWriteMMIOAnnotations(outputDir);
      llvm::sys::path::append(loadMemWriteMMIOPath, "post-fame-loadmem-write-mmio.mlir");
      llvm::sys::path::append(loadMemWriteMMIOAnnotations, "post-fame-loadmem-write-mmio-all.json");
      std::error_code loadMemWriteMMIOError;
      llvm::raw_fd_ostream loadMemWriteMMIOOut(loadMemWriteMMIOPath, loadMemWriteMMIOError);
      if (loadMemWriteMMIOError)
        return fail("cannot write LoadMem write MMIO: " + loadMemWriteMMIOError.message());
      module->print(loadMemWriteMMIOOut); loadMemWriteMMIOOut << '\n'; loadMemWriteMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemWriteMMIOAnnotations, error)))
        return fail("LoadMem write MMIO annotations: " + error);
      llvm::outs() << "Mapped CIRCT LoadMem write-address MMIO bank in " << loadMemWriteMMIOPath << '\n';
      if (failed(goldengate::addLoadMemWriteData(circuit, error)))
        return fail("LoadMem write data: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem write data produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemWriteDataPath(outputDir), loadMemWriteDataAnnotations(outputDir);
      llvm::sys::path::append(loadMemWriteDataPath, "post-fame-loadmem-write-data.mlir");
      llvm::sys::path::append(loadMemWriteDataAnnotations, "post-fame-loadmem-write-data-all.json");
      std::error_code loadMemWriteDataError;
      llvm::raw_fd_ostream loadMemWriteDataOut(loadMemWriteDataPath, loadMemWriteDataError);
      if (loadMemWriteDataError)
        return fail("cannot write LoadMem write data: " + loadMemWriteDataError.message());
      module->print(loadMemWriteDataOut); loadMemWriteDataOut << '\n'; loadMemWriteDataOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemWriteDataAnnotations, error)))
        return fail("LoadMem write data annotations: " + error);
      llvm::outs() << "Mapped CIRCT LoadMem write-data FIFO in " << loadMemWriteDataPath << '\n';
      if (failed(goldengate::addLoadMemReadRequests(circuit, error)))
        return fail("LoadMem read requests: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem read requests produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemReadRequestsPath(outputDir), loadMemReadRequestsAnnotations(outputDir);
      llvm::sys::path::append(loadMemReadRequestsPath, "post-fame-loadmem-read-requests.mlir");
      llvm::sys::path::append(loadMemReadRequestsAnnotations, "post-fame-loadmem-read-requests-all.json");
      std::error_code loadMemReadRequestsError;
      llvm::raw_fd_ostream loadMemReadRequestsOut(loadMemReadRequestsPath, loadMemReadRequestsError);
      if (loadMemReadRequestsError)
        return fail("cannot write LoadMem read requests: " + loadMemReadRequestsError.message());
      module->print(loadMemReadRequestsOut); loadMemReadRequestsOut << '\n'; loadMemReadRequestsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemReadRequestsAnnotations, error)))
        return fail("LoadMem read requests annotations: " + error);
      llvm::outs() << "Queued CIRCT LoadMem read-address requests in " << loadMemReadRequestsPath << '\n';
      if (failed(goldengate::addLoadMemReadData(circuit, error)))
        return fail("LoadMem read data: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem read data produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemReadDataPath(outputDir), loadMemReadDataAnnotations(outputDir);
      llvm::sys::path::append(loadMemReadDataPath, "post-fame-loadmem-read-data.mlir");
      llvm::sys::path::append(loadMemReadDataAnnotations, "post-fame-loadmem-read-data-all.json");
      std::error_code loadMemReadDataError;
      llvm::raw_fd_ostream loadMemReadDataOut(loadMemReadDataPath, loadMemReadDataError);
      if (loadMemReadDataError)
        return fail("cannot write LoadMem read data: " + loadMemReadDataError.message());
      module->print(loadMemReadDataOut); loadMemReadDataOut << '\n'; loadMemReadDataOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemReadDataAnnotations, error)))
        return fail("LoadMem read data annotations: " + error);
      llvm::outs() << "Mapped CIRCT LoadMem read-data FIFO in " << loadMemReadDataPath << '\n';
      if (failed(goldengate::mapLoadMemControl(circuit, 25, 12, error)))
        return fail("LoadMem control: " + error);
      if (failed(mlir::verify(*module)))
        return fail("LoadMem control produced invalid FIRRTL IR");
      llvm::SmallString<256> loadMemControlPath(outputDir), loadMemControlAnnotations(outputDir);
      llvm::sys::path::append(loadMemControlPath, "post-fame-loadmem-control.mlir");
      llvm::sys::path::append(loadMemControlAnnotations, "post-fame-loadmem-control-all.json");
      std::error_code loadMemControlError;
      llvm::raw_fd_ostream loadMemControlOut(loadMemControlPath, loadMemControlError);
      if (loadMemControlError)
        return fail("cannot write LoadMem control: " + loadMemControlError.message());
      module->print(loadMemControlOut); loadMemControlOut << '\n'; loadMemControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, loadMemControlAnnotations, error)))
        return fail("LoadMem control annotations: " + error);
      llvm::outs() << "Mapped CIRCT LoadMem MCRFile transactions in " << loadMemControlPath << '\n';
      if (failed(goldengate::addControlErrorSlave(circuit, 25, 12, error)))
        return fail("control error slave: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control error slave produced invalid FIRRTL IR");
      llvm::SmallString<256> controlErrorPath(outputDir), controlErrorAnnotations(outputDir);
      llvm::sys::path::append(controlErrorPath, "post-fame-control-error-slave.mlir");
      llvm::sys::path::append(controlErrorAnnotations, "post-fame-control-error-slave-all.json");
      std::error_code controlErrorWriteError;
      llvm::raw_fd_ostream controlErrorOut(controlErrorPath, controlErrorWriteError);
      if (controlErrorWriteError)
        return fail("cannot write control error slave: " + controlErrorWriteError.message());
      module->print(controlErrorOut); controlErrorOut << '\n'; controlErrorOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlErrorAnnotations, error)))
        return fail("control error slave annotations: " + error);
      llvm::outs() << "Mapped CIRCT control decode-error endpoint in " << controlErrorPath << '\n';
      // FireSimRocketConfig/U250 HasWidgets sorted allocation. Keep the full
      // catalog, including widgets whose CIRCT implementations are pending.
      const goldengate::ControlMMIORegion controlRegions[]{
          {"BlockDevBridgeModule_0", 0x000, 0x80}, {"FASEDMemoryTimingModel_0", 0x080, 0x80},
          {"TracerVBridgeModule_0", 0x100, 0x40}, {"TSIBridgeModule_0", 0x140, 0x40},
          {"LoadMemWidget_0", 0x180, 0x40}, {"PeekPokeBridgeModule_0", 0x1c0, 0x20},
          {"UARTBridgeModule_0", 0x1e0, 0x20}, {"ClockBridgeModule_0", 0x200, 0x20},
          {"SimulationMaster_0", 0x220, 0x10}, {"ResetPulseBridgeModule_0", 0x230, 0x08},
          {"CPUManagedStreamEngine_0", 0x238, 0x04}};
      if (failed(goldengate::addControlAddressDecode(circuit, 25, controlRegions, error)))
        return fail("control address decoder: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control address decoder produced invalid FIRRTL IR");
      llvm::SmallString<256> controlDecodePath(outputDir), controlDecodeAnnotations(outputDir);
      llvm::sys::path::append(controlDecodePath, "post-fame-control-address-decode.mlir");
      llvm::sys::path::append(controlDecodeAnnotations, "post-fame-control-address-decode-all.json");
      std::error_code controlDecodeWriteError;
      llvm::raw_fd_ostream controlDecodeOut(controlDecodePath, controlDecodeWriteError);
      if (controlDecodeWriteError)
        return fail("cannot write control address decoder: " + controlDecodeWriteError.message());
      module->print(controlDecodeOut); controlDecodeOut << '\n'; controlDecodeOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlDecodeAnnotations, error)))
        return fail("control decoder annotations: " + error);
      llvm::outs() << "Mapped CIRCT control address decoder in " << controlDecodePath << '\n';
      if (failed(goldengate::addControlWriteRoute(circuit, error)))
        return fail("control write route: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control write route produced invalid FIRRTL IR");
      llvm::SmallString<256> controlRoutePath(outputDir), controlRouteAnnotations(outputDir);
      llvm::sys::path::append(controlRoutePath, "post-fame-control-write-route.mlir");
      llvm::sys::path::append(controlRouteAnnotations, "post-fame-control-write-route-all.json");
      std::error_code controlRouteWriteError;
      llvm::raw_fd_ostream controlRouteOut(controlRoutePath, controlRouteWriteError);
      if (controlRouteWriteError)
        return fail("cannot write control write route: " + controlRouteWriteError.message());
      module->print(controlRouteOut); controlRouteOut << '\n'; controlRouteOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlRouteAnnotations, error)))
        return fail("control write route annotations: " + error);
      llvm::outs() << "Queued CIRCT control AW routes through accepted W.last in " << controlRoutePath << '\n';
      if (failed(goldengate::addControlWriteDispatch(circuit, error)))
        return fail("control write dispatch: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control write dispatch produced invalid FIRRTL IR");
      llvm::SmallString<256> controlDispatchPath(outputDir), controlDispatchAnnotations(outputDir);
      llvm::sys::path::append(controlDispatchPath, "post-fame-control-write-dispatch.mlir");
      llvm::sys::path::append(controlDispatchAnnotations, "post-fame-control-write-dispatch-all.json");
      std::error_code controlDispatchWriteError;
      llvm::raw_fd_ostream controlDispatchOut(controlDispatchPath, controlDispatchWriteError);
      if (controlDispatchWriteError)
        return fail("cannot write control write dispatch: " + controlDispatchWriteError.message());
      module->print(controlDispatchOut); controlDispatchOut << '\n'; controlDispatchOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlDispatchAnnotations, error)))
        return fail("control dispatch annotations: " + error);
      llvm::outs() << "Dispatched CIRCT control AW/W requests and connected the error endpoint in " << controlDispatchPath << '\n';
      if (failed(goldengate::bindControlWidgetWrites(circuit, error)))
        return fail("control widget writes: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control widget writes produced invalid FIRRTL IR");
      llvm::SmallString<256> widgetWritesPath(outputDir), widgetWritesAnnotations(outputDir);
      llvm::sys::path::append(widgetWritesPath, "post-fame-control-widget-writes.mlir");
      llvm::sys::path::append(widgetWritesAnnotations, "post-fame-control-widget-writes-all.json");
      std::error_code widgetWritesWriteError;
      llvm::raw_fd_ostream widgetWritesOut(widgetWritesPath, widgetWritesWriteError);
      if (widgetWritesWriteError)
        return fail("cannot write control widget writes: " + widgetWritesWriteError.message());
      module->print(widgetWritesOut); widgetWritesOut << '\n'; widgetWritesOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, widgetWritesAnnotations, error)))
        return fail("control widget write annotations: " + error);
      llvm::outs() << "Bound CIRCT control AW/W requests to seven implemented widgets in " << widgetWritesPath << '\n';
      if (failed(goldengate::addControlReadDispatch(circuit, error)))
        return fail("control read dispatch: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control read dispatch produced invalid FIRRTL IR");
      llvm::SmallString<256> controlReadsPath(outputDir), controlReadsAnnotations(outputDir);
      llvm::sys::path::append(controlReadsPath, "post-fame-control-read-dispatch.mlir");
      llvm::sys::path::append(controlReadsAnnotations, "post-fame-control-read-dispatch-all.json");
      std::error_code controlReadsWriteError;
      llvm::raw_fd_ostream controlReadsOut(controlReadsPath, controlReadsWriteError);
      if (controlReadsWriteError)
        return fail("cannot write control read dispatch: " + controlReadsWriteError.message());
      module->print(controlReadsOut); controlReadsOut << '\n'; controlReadsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlReadsAnnotations, error)))
        return fail("control read annotations: " + error);
      llvm::outs() << "Dispatched CIRCT control AR requests to seven widgets and the error endpoint in " << controlReadsPath << '\n';
      if (failed(goldengate::addControlReadTracker(circuit, error)))
        return fail("control read tracker: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control read tracker produced invalid FIRRTL IR");
      llvm::SmallString<256> readTrackerPath(outputDir), readTrackerAnnotations(outputDir);
      llvm::sys::path::append(readTrackerPath, "post-fame-control-read-tracker.mlir");
      llvm::sys::path::append(readTrackerAnnotations, "post-fame-control-read-tracker-all.json");
      std::error_code readTrackerWriteError;
      llvm::raw_fd_ostream readTrackerOut(readTrackerPath, readTrackerWriteError);
      if (readTrackerWriteError)
        return fail("cannot write control read tracker: " + readTrackerWriteError.message());
      module->print(readTrackerOut); readTrackerOut << '\n'; readTrackerOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, readTrackerAnnotations, error)))
        return fail("control read tracker annotations: " + error);
      llvm::outs() << "Tracked CIRCT control AR transactions and accepted final R beats in " << readTrackerPath << '\n';
      if (failed(goldengate::addControlReadArbiter(circuit, error)))
        return fail("control read arbiter: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control read arbiter produced invalid FIRRTL IR");
      llvm::SmallString<256> readArbiterPath(outputDir), readArbiterAnnotations(outputDir);
      llvm::sys::path::append(readArbiterPath, "post-fame-control-read-arbiter.mlir");
      llvm::sys::path::append(readArbiterAnnotations, "post-fame-control-read-arbiter-all.json");
      std::error_code readArbiterWriteError;
      llvm::raw_fd_ostream readArbiterOut(readArbiterPath, readArbiterWriteError);
      if (readArbiterWriteError)
        return fail("cannot write control read arbiter: " + readArbiterWriteError.message());
      module->print(readArbiterOut); readArbiterOut << '\n'; readArbiterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, readArbiterAnnotations, error)))
        return fail("control read arbiter annotations: " + error);
      llvm::outs() << "Arbitrated CIRCT control R bursts and connected tracker retirement in " << readArbiterPath << '\n';
      if (failed(goldengate::addControlWriteArbiter(circuit, error)))
        return fail("control write arbiter: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control write arbiter produced invalid FIRRTL IR");
      llvm::SmallString<256> writeArbiterPath(outputDir), writeArbiterAnnotations(outputDir);
      llvm::sys::path::append(writeArbiterPath, "post-fame-control-write-arbiter.mlir");
      llvm::sys::path::append(writeArbiterAnnotations, "post-fame-control-write-arbiter-all.json");
      std::error_code writeArbiterWriteError;
      llvm::raw_fd_ostream writeArbiterOut(writeArbiterPath, writeArbiterWriteError);
      if (writeArbiterWriteError)
        return fail("cannot write control write arbiter: " + writeArbiterWriteError.message());
      module->print(writeArbiterOut); writeArbiterOut << '\n'; writeArbiterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, writeArbiterAnnotations, error)))
        return fail("control write arbiter annotations: " + error);
      llvm::outs() << "Arbitrated CIRCT control B responses and exposed write retirement in " << writeArbiterPath << '\n';
      if (failed(goldengate::addControlWriteTracker(circuit, error)))
        return fail("control write tracker: " + error);
      if (failed(mlir::verify(*module)))
        return fail("control write tracker produced invalid FIRRTL IR");
      llvm::SmallString<256> writeTrackerPath(outputDir), writeTrackerAnnotations(outputDir);
      llvm::sys::path::append(writeTrackerPath, "post-fame-control-write-tracker.mlir");
      llvm::sys::path::append(writeTrackerAnnotations, "post-fame-control-write-tracker-all.json");
      std::error_code writeTrackerWriteError;
      llvm::raw_fd_ostream writeTrackerOut(writeTrackerPath, writeTrackerWriteError);
      if (writeTrackerWriteError)
        return fail("cannot write control write tracker: " + writeTrackerWriteError.message());
      module->print(writeTrackerOut); writeTrackerOut << '\n'; writeTrackerOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, writeTrackerAnnotations, error)))
        return fail("control write tracker annotations: " + error);
      llvm::outs() << "Tracked CIRCT control AW transactions and accepted B responses in " << writeTrackerPath << '\n';
      if (failed(goldengate::addSimulationMasterBank(circuit, error)))
        return fail("SimulationMaster bank: " + error);
      if (failed(mlir::verify(*module)))
        return fail("SimulationMaster bank produced invalid FIRRTL IR");
      llvm::SmallString<256> masterPath(outputDir), masterAnnotations(outputDir);
      llvm::sys::path::append(masterPath, "post-fame-simulation-master.mlir");
      llvm::sys::path::append(masterAnnotations, "post-fame-simulation-master-all.json");
      std::error_code masterWriteError;
      llvm::raw_fd_ostream masterOut(masterPath, masterWriteError);
      if (masterWriteError)
        return fail("cannot write SimulationMaster bank: " + masterWriteError.message());
      module->print(masterOut); masterOut << '\n'; masterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, masterAnnotations, error)))
        return fail("SimulationMaster bank annotations: " + error);
      llvm::outs() << "Mapped CIRCT SimulationMaster initialization and presence registers in " << masterPath << '\n';
      if (failed(goldengate::mapSimulationMasterControl(circuit, 25, 12, error)))
        return fail("SimulationMaster MCRFile: " + error);
      if (failed(mlir::verify(*module)))
        return fail("SimulationMaster MCRFile produced invalid FIRRTL IR");
      llvm::SmallString<256> masterControlPath(outputDir), masterControlAnnotations(outputDir);
      llvm::sys::path::append(masterControlPath, "post-fame-simulation-master-control.mlir");
      llvm::sys::path::append(masterControlAnnotations, "post-fame-simulation-master-control-all.json");
      std::error_code masterControlError;
      llvm::raw_fd_ostream masterControlOut(masterControlPath, masterControlError);
      if (masterControlError) return fail("cannot write SimulationMaster MCRFile: " + masterControlError.message());
      module->print(masterControlOut); masterControlOut << '\n'; masterControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, masterControlAnnotations, error)))
        return fail("SimulationMaster MCRFile annotations: " + error);
      llvm::outs() << "Mapped CIRCT SimulationMaster MCRFile transactions in " << masterControlPath << '\n';
      if (failed(goldengate::bindSimulationMasterControl(circuit, error)))
        return fail("SimulationMaster control binding: " + error);
      if (failed(mlir::verify(*module)))
        return fail("SimulationMaster control binding produced invalid FIRRTL IR");
      llvm::SmallString<256> masterBoundPath(outputDir), masterBoundAnnotations(outputDir);
      llvm::sys::path::append(masterBoundPath, "post-fame-simulation-master-bound.mlir");
      llvm::sys::path::append(masterBoundAnnotations, "post-fame-simulation-master-bound-all.json");
      std::error_code masterBoundError;
      llvm::raw_fd_ostream masterBoundOut(masterBoundPath, masterBoundError);
      if (masterBoundError) return fail("cannot write SimulationMaster binding: " + masterBoundError.message());
      module->print(masterBoundOut); masterBoundOut << '\n'; masterBoundOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, masterBoundAnnotations, error)))
        return fail("SimulationMaster binding annotations: " + error);
      llvm::outs() << "Bound CIRCT SimulationMaster requests and responses to control slave 8 in " << masterBoundPath << '\n';
      if (failed(goldengate::addTSITokenEngine(circuit, error)))
        return fail("TSI token mapping: " + error);
      if (failed(mlir::verify(*module))) return fail("TSI token mapping produced invalid FIRRTL IR");
      llvm::SmallString<256> tsiPath(outputDir), tsiAnnotations(outputDir);
      llvm::sys::path::append(tsiPath, "post-fame-tsi-token.mlir");
      llvm::sys::path::append(tsiAnnotations, "post-fame-tsi-token-all.json");
      std::error_code tsiWriteError;
      llvm::raw_fd_ostream tsiOut(tsiPath, tsiWriteError);
      if (tsiWriteError) return fail("cannot write TSI token mapping: " + tsiWriteError.message());
      module->print(tsiOut); tsiOut << '\n'; tsiOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tsiAnnotations, error)))
        return fail("TSI token mapping annotations: " + error);
      llvm::outs() << "Mapped CIRCT TSI cycle scheduling and queue token gates in " << tsiPath << '\n';
      if (failed(goldengate::addTSIWordQueues(circuit, error)))
        return fail("TSI word queues: " + error);
      if (failed(mlir::verify(*module))) return fail("TSI word queues produced invalid FIRRTL IR");
      llvm::SmallString<256> tsiQueuesPath(outputDir), tsiQueuesAnnotations(outputDir);
      llvm::sys::path::append(tsiQueuesPath, "post-fame-tsi-word-queues.mlir");
      llvm::sys::path::append(tsiQueuesAnnotations, "post-fame-tsi-word-queues-all.json");
      std::error_code tsiQueuesWriteError;
      llvm::raw_fd_ostream tsiQueuesOut(tsiQueuesPath, tsiQueuesWriteError);
      if (tsiQueuesWriteError) return fail("cannot write TSI word queues: " + tsiQueuesWriteError.message());
      module->print(tsiQueuesOut); tsiQueuesOut << '\n'; tsiQueuesOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tsiQueuesAnnotations, error)))
        return fail("TSI word queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT TSI words in two 16-entry queues in " << tsiQueuesPath << '\n';
      if (failed(goldengate::addTSIMMIOBank(circuit, error)))
        return fail("TSI MMIO bank: " + error);
      if (failed(mlir::verify(*module))) return fail("TSI MMIO bank produced invalid FIRRTL IR");
      llvm::SmallString<256> tsiMMIOPath(outputDir), tsiMMIOAnnotations(outputDir);
      llvm::sys::path::append(tsiMMIOPath, "post-fame-tsi-mmio.mlir");
      llvm::sys::path::append(tsiMMIOAnnotations, "post-fame-tsi-mmio-all.json");
      std::error_code tsiMMIOWriteError;
      llvm::raw_fd_ostream tsiMMIOOut(tsiMMIOPath, tsiMMIOWriteError);
      if (tsiMMIOWriteError) return fail("cannot write TSI MMIO bank: " + tsiMMIOWriteError.message());
      module->print(tsiMMIOOut); tsiMMIOOut << '\n'; tsiMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tsiMMIOAnnotations, error)))
        return fail("TSI MMIO bank annotations: " + error);
      llvm::outs() << "Mapped CIRCT TSI queue and scheduler MMIO registers in " << tsiMMIOPath << '\n';
      if (failed(goldengate::mapTSIBridgeControl(circuit, 25, 12, error)))
        return fail("TSI MCRFile control: " + error);
      if (failed(mlir::verify(*module))) return fail("TSI MCRFile control produced invalid FIRRTL IR");
      llvm::SmallString<256> tsiControlPath(outputDir), tsiControlAnnotations(outputDir);
      llvm::sys::path::append(tsiControlPath, "post-fame-tsi-control.mlir");
      llvm::sys::path::append(tsiControlAnnotations, "post-fame-tsi-control-all.json");
      std::error_code tsiControlWriteError;
      llvm::raw_fd_ostream tsiControlOut(tsiControlPath, tsiControlWriteError);
      if (tsiControlWriteError) return fail("cannot write TSI MCRFile control: " + tsiControlWriteError.message());
      module->print(tsiControlOut); tsiControlOut << '\n'; tsiControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tsiControlAnnotations, error)))
        return fail("TSI MCRFile control annotations: " + error);
      llvm::outs() << "Mapped CIRCT TSI MCRFile transactions in " << tsiControlPath << '\n';
      if (failed(goldengate::bindTSIBridgeControl(circuit, error)))
        return fail("TSI control binding: " + error);
      if (failed(mlir::verify(*module))) return fail("TSI control binding produced invalid FIRRTL IR");
      llvm::SmallString<256> tsiBoundPath(outputDir), tsiBoundAnnotations(outputDir);
      llvm::sys::path::append(tsiBoundPath, "post-fame-tsi-bound.mlir");
      llvm::sys::path::append(tsiBoundAnnotations, "post-fame-tsi-bound-all.json");
      std::error_code tsiBoundError;
      llvm::raw_fd_ostream tsiBoundOut(tsiBoundPath, tsiBoundError);
      if (tsiBoundError) return fail("cannot write TSI binding: " + tsiBoundError.message());
      module->print(tsiBoundOut); tsiBoundOut << '\n'; tsiBoundOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, tsiBoundAnnotations, error)))
        return fail("TSI binding annotations: " + error);
      llvm::outs() << "Bound CIRCT TSI requests and responses to control slave 3 in " << tsiBoundPath << '\n';
      if (failed(goldengate::addBlockDevTokenEngine(circuit, error)))
        return fail("BlockDev token engine: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev token engine produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevPath(outputDir), blockDevAnnotations(outputDir);
      llvm::sys::path::append(blockDevPath, "post-fame-blockdev-token.mlir");
      llvm::sys::path::append(blockDevAnnotations, "post-fame-blockdev-token-all.json");
      std::error_code blockDevError;
      llvm::raw_fd_ostream blockDevOut(blockDevPath, blockDevError);
      if (blockDevError) return fail("cannot write BlockDev token engine: " + blockDevError.message());
      module->print(blockDevOut); blockDevOut << '\n'; blockDevOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevAnnotations, error)))
        return fail("BlockDev token annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev token gates, target cycle counter and write-beat tracking in " << blockDevPath << '\n';
      if (failed(goldengate::addBlockDevRequestQueue(circuit, error)))
        return fail("BlockDev request queue: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev request queue produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevQueuePath(outputDir), blockDevQueueAnnotations(outputDir);
      llvm::sys::path::append(blockDevQueuePath, "post-fame-blockdev-request-queue.mlir");
      llvm::sys::path::append(blockDevQueueAnnotations, "post-fame-blockdev-request-queue-all.json");
      std::error_code blockDevQueueError;
      llvm::raw_fd_ostream blockDevQueueOut(blockDevQueuePath, blockDevQueueError);
      if (blockDevQueueError) return fail("cannot write BlockDev request queue: " + blockDevQueueError.message());
      module->print(blockDevQueueOut); blockDevQueueOut << '\n'; blockDevQueueOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevQueueAnnotations, error)))
        return fail("BlockDev request queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT BlockDev requests in a 10-entry queue in " << blockDevQueuePath << '\n';
      if (failed(goldengate::addBlockDevDataQueue(circuit, error)))
        return fail("BlockDev data queue: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev data queue produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevDataPath(outputDir), blockDevDataAnnotations(outputDir);
      llvm::sys::path::append(blockDevDataPath, "post-fame-blockdev-data-queue.mlir");
      llvm::sys::path::append(blockDevDataAnnotations, "post-fame-blockdev-data-queue-all.json");
      std::error_code blockDevDataError;
      llvm::raw_fd_ostream blockDevDataOut(blockDevDataPath, blockDevDataError);
      if (blockDevDataError) return fail("cannot write BlockDev data queue: " + blockDevDataError.message());
      module->print(blockDevDataOut); blockDevDataOut << '\n'; blockDevDataOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevDataAnnotations, error)))
        return fail("BlockDev data queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT BlockDev data in a 32-entry queue in " << blockDevDataPath << '\n';
      if (failed(goldengate::addBlockDevReadResponseQueue(circuit, error)))
        return fail("BlockDev read-response queue: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev read-response queue produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevReadResponsePath(outputDir), blockDevReadResponseAnnotations(outputDir);
      llvm::sys::path::append(blockDevReadResponsePath, "post-fame-blockdev-read-response-queue.mlir");
      llvm::sys::path::append(blockDevReadResponseAnnotations, "post-fame-blockdev-read-response-queue-all.json");
      std::error_code blockDevReadResponseError;
      llvm::raw_fd_ostream blockDevReadResponseOut(blockDevReadResponsePath, blockDevReadResponseError);
      if (blockDevReadResponseError) return fail("cannot write BlockDev read-response queue: " + blockDevReadResponseError.message());
      module->print(blockDevReadResponseOut); blockDevReadResponseOut << '\n'; blockDevReadResponseOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevReadResponseAnnotations, error)))
        return fail("BlockDev read-response queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT BlockDev read responses in a 32-entry queue in " << blockDevReadResponsePath << '\n';
      if (failed(goldengate::addBlockDevWriteAckQueue(circuit, error)))
        return fail("BlockDev write-ack queue: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev write-ack queue produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevWriteAckPath(outputDir), blockDevWriteAckAnnotations(outputDir);
      llvm::sys::path::append(blockDevWriteAckPath, "post-fame-blockdev-write-ack-queue.mlir");
      llvm::sys::path::append(blockDevWriteAckAnnotations, "post-fame-blockdev-write-ack-queue-all.json");
      std::error_code blockDevWriteAckError;
      llvm::raw_fd_ostream blockDevWriteAckOut(blockDevWriteAckPath, blockDevWriteAckError);
      if (blockDevWriteAckError) return fail("cannot write BlockDev write-ack queue: " + blockDevWriteAckError.message());
      module->print(blockDevWriteAckOut); blockDevWriteAckOut << '\n'; blockDevWriteAckOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevWriteAckAnnotations, error)))
        return fail("BlockDev write-ack queue annotations: " + error);
      llvm::outs() << "Buffered CIRCT BlockDev write acknowledgements in a 4-entry queue in " << blockDevWriteAckPath << '\n';
      if (failed(goldengate::addBlockDevMMIOBank(circuit, error)))
        return fail("BlockDev MMIO bank: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev MMIO bank produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevMMIOPath(outputDir), blockDevMMIOAnnotations(outputDir);
      llvm::sys::path::append(blockDevMMIOPath, "post-fame-blockdev-mmio.mlir");
      llvm::sys::path::append(blockDevMMIOAnnotations, "post-fame-blockdev-mmio-all.json");
      std::error_code blockDevMMIOError;
      llvm::raw_fd_ostream blockDevMMIOOut(blockDevMMIOPath, blockDevMMIOError);
      if (blockDevMMIOError) return fail("cannot write BlockDev MMIO bank: " + blockDevMMIOError.message());
      module->print(blockDevMMIOOut); blockDevMMIOOut << '\n'; blockDevMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevMMIOAnnotations, error)))
        return fail("BlockDev MMIO bank annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev 26-word MMIO bank in " << blockDevMMIOPath << '\n';
      if (failed(goldengate::mapBlockDevBridgeControl(circuit, 25, 12, error)))
        return fail("BlockDev control transport: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev control transport produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevControlPath(outputDir), blockDevControlAnnotations(outputDir);
      llvm::sys::path::append(blockDevControlPath, "post-fame-blockdev-control.mlir");
      llvm::sys::path::append(blockDevControlAnnotations, "post-fame-blockdev-control-all.json");
      std::error_code blockDevControlError;
      llvm::raw_fd_ostream blockDevControlOut(blockDevControlPath, blockDevControlError);
      if (blockDevControlError) return fail("cannot write BlockDev control transport: " + blockDevControlError.message());
      module->print(blockDevControlOut); blockDevControlOut << '\n'; blockDevControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevControlAnnotations, error)))
        return fail("BlockDev control transport annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev MCRFile transactions in " << blockDevControlPath << '\n';
      if (failed(goldengate::bindBlockDevBridgeControl(circuit, error)))
        return fail("BlockDev control binding: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev control binding produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevBoundPath(outputDir), blockDevBoundAnnotations(outputDir);
      llvm::sys::path::append(blockDevBoundPath, "post-fame-blockdev-bound.mlir");
      llvm::sys::path::append(blockDevBoundAnnotations, "post-fame-blockdev-bound-all.json");
      std::error_code blockDevBoundError;
      llvm::raw_fd_ostream blockDevBoundOut(blockDevBoundPath, blockDevBoundError);
      if (blockDevBoundError) return fail("cannot write BlockDev control binding: " + blockDevBoundError.message());
      module->print(blockDevBoundOut); blockDevBoundOut << '\n'; blockDevBoundOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevBoundAnnotations, error)))
        return fail("BlockDev control binding annotations: " + error);
      llvm::outs() << "Bound CIRCT BlockDev requests and responses to control slave 0 in " << blockDevBoundPath << '\n';
      if (failed(goldengate::addBlockDevWriteLatency(circuit, error)))
        return fail("BlockDev write latency: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev write latency produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevWriteLatencyPath(outputDir), blockDevWriteLatencyAnnotations(outputDir);
      llvm::sys::path::append(blockDevWriteLatencyPath, "post-fame-blockdev-write-latency.mlir");
      llvm::sys::path::append(blockDevWriteLatencyAnnotations, "post-fame-blockdev-write-latency-all.json");
      std::error_code blockDevWriteLatencyError;
      llvm::raw_fd_ostream blockDevWriteLatencyOut(blockDevWriteLatencyPath, blockDevWriteLatencyError);
      if (blockDevWriteLatencyError) return fail("cannot write BlockDev write latency: " + blockDevWriteLatencyError.message());
      module->print(blockDevWriteLatencyOut); blockDevWriteLatencyOut << '\n'; blockDevWriteLatencyOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevWriteLatencyAnnotations, error)))
        return fail("BlockDev write latency annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev one-entry write latency pipe in " << blockDevWriteLatencyPath << '\n';
      if (failed(goldengate::addBlockDevReadLatency(circuit, error)))
        return fail("BlockDev read latency: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev read latency produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevReadLatencyPath(outputDir), blockDevReadLatencyAnnotations(outputDir);
      llvm::sys::path::append(blockDevReadLatencyPath, "post-fame-blockdev-read-latency.mlir");
      llvm::sys::path::append(blockDevReadLatencyAnnotations, "post-fame-blockdev-read-latency-all.json");
      std::error_code blockDevReadLatencyError;
      llvm::raw_fd_ostream blockDevReadLatencyOut(blockDevReadLatencyPath, blockDevReadLatencyError);
      if (blockDevReadLatencyError) return fail("cannot write BlockDev read latency: " + blockDevReadLatencyError.message());
      module->print(blockDevReadLatencyOut); blockDevReadLatencyOut << '\n'; blockDevReadLatencyOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevReadLatencyAnnotations, error)))
        return fail("BlockDev read latency annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev one-entry read latency pipe in " << blockDevReadLatencyPath << '\n';
      if (failed(goldengate::addBlockDevResponseScheduler(circuit, error)))
        return fail("BlockDev response scheduler: " + error);
      if (failed(mlir::verify(*module))) return fail("BlockDev response scheduler produced invalid FIRRTL IR");
      llvm::SmallString<256> blockDevResponseSchedulerPath(outputDir), blockDevResponseSchedulerAnnotations(outputDir);
      llvm::sys::path::append(blockDevResponseSchedulerPath, "post-fame-blockdev-response-scheduler.mlir");
      llvm::sys::path::append(blockDevResponseSchedulerAnnotations, "post-fame-blockdev-response-scheduler-all.json");
      std::error_code blockDevResponseSchedulerError;
      llvm::raw_fd_ostream blockDevResponseSchedulerOut(blockDevResponseSchedulerPath, blockDevResponseSchedulerError);
      if (blockDevResponseSchedulerError) return fail("cannot write BlockDev response scheduler: " + blockDevResponseSchedulerError.message());
      module->print(blockDevResponseSchedulerOut); blockDevResponseSchedulerOut << '\n'; blockDevResponseSchedulerOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, blockDevResponseSchedulerAnnotations, error)))
        return fail("BlockDev response scheduler annotations: " + error);
      llvm::outs() << "Mapped CIRCT BlockDev response scheduler in " << blockDevResponseSchedulerPath << '\n';
      if (failed(goldengate::addFASEDTokenEngine(circuit, error)))
        return fail("FASED token engine: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED token engine produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedTokenPath(outputDir), fasedTokenAnnotations(outputDir);
      llvm::sys::path::append(fasedTokenPath, "post-fame-fased-token.mlir");
      llvm::sys::path::append(fasedTokenAnnotations, "post-fame-fased-token-all.json");
      std::error_code fasedTokenError;
      llvm::raw_fd_ostream fasedTokenOut(fasedTokenPath, fasedTokenError);
      if (fasedTokenError) return fail("cannot write FASED token engine: " + fasedTokenError.message());
      module->print(fasedTokenOut); fasedTokenOut << '\n'; fasedTokenOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedTokenAnnotations, error)))
        return fail("FASED token engine annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED token engine in " << fasedTokenPath << '\n';
      if (failed(goldengate::addFASEDHostOutstanding(circuit, error)))
        return fail("FASED host outstanding: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED host outstanding produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedHostPath(outputDir), fasedHostAnnotations(outputDir);
      llvm::sys::path::append(fasedHostPath, "post-fame-fased-host-outstanding.mlir");
      llvm::sys::path::append(fasedHostAnnotations, "post-fame-fased-host-outstanding-all.json");
      std::error_code fasedHostError;
      llvm::raw_fd_ostream fasedHostOut(fasedHostPath, fasedHostError);
      if (fasedHostError) return fail("cannot write FASED host outstanding: " + fasedHostError.message());
      module->print(fasedHostOut); fasedHostOut << '\n'; fasedHostOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedHostAnnotations, error)))
        return fail("FASED host outstanding annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host outstanding counters in " << fasedHostPath << '\n';
      if (failed(goldengate::addFASEDIngressAWQueue(circuit, error)))
        return fail("FASED AW ingress queue: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED AW ingress produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedAWPath(outputDir), fasedAWAnnotations(outputDir);
      llvm::sys::path::append(fasedAWPath, "post-fame-fased-ingress-aw.mlir");
      llvm::sys::path::append(fasedAWAnnotations, "post-fame-fased-ingress-aw-all.json");
      std::error_code fasedAWError;
      llvm::raw_fd_ostream fasedAWOut(fasedAWPath, fasedAWError);
      if (fasedAWError) return fail("cannot write FASED AW ingress: " + fasedAWError.message());
      module->print(fasedAWOut); fasedAWOut << '\n'; fasedAWOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedAWAnnotations, error)))
        return fail("FASED AW ingress annotations: " + error);
      llvm::outs() << "Buffered CIRCT FASED AW requests in a 10-entry ingress queue in " << fasedAWPath << '\n';
      if (failed(goldengate::addFASEDIngressWQueue(circuit, error)))
        return fail("FASED W ingress queue: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED W ingress produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWPath(outputDir), fasedWAnnotations(outputDir);
      llvm::sys::path::append(fasedWPath, "post-fame-fased-ingress-w.mlir");
      llvm::sys::path::append(fasedWAnnotations, "post-fame-fased-ingress-w-all.json");
      std::error_code fasedWError;
      llvm::raw_fd_ostream fasedWOut(fasedWPath, fasedWError);
      if (fasedWError) return fail("cannot write FASED W ingress: " + fasedWError.message());
      module->print(fasedWOut); fasedWOut << '\n'; fasedWOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWAnnotations, error)))
        return fail("FASED W ingress annotations: " + error);
      llvm::outs() << "Buffered CIRCT FASED W beats in a 16-entry ingress queue in " << fasedWPath << '\n';
      if (failed(goldengate::addFASEDIngressARQueue(circuit, error)))
        return fail("FASED AR ingress queue: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED AR ingress produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedARPath(outputDir), fasedARAnnotations(outputDir);
      llvm::sys::path::append(fasedARPath, "post-fame-fased-ingress-ar.mlir");
      llvm::sys::path::append(fasedARAnnotations, "post-fame-fased-ingress-ar-all.json");
      std::error_code fasedARError;
      llvm::raw_fd_ostream fasedAROut(fasedARPath, fasedARError);
      if (fasedARError) return fail("cannot write FASED AR ingress: " + fasedARError.message());
      module->print(fasedAROut); fasedAROut << '\n'; fasedAROut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedARAnnotations, error)))
        return fail("FASED AR ingress annotations: " + error);
      llvm::outs() << "Buffered CIRCT FASED AR requests in a 4-entry ingress queue in " << fasedARPath << '\n';
      if (failed(goldengate::addFASEDIngressCredits(circuit, error)))
        return fail("FASED ingress credits: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED ingress credits produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedCreditsPath(outputDir), fasedCreditsAnnotations(outputDir);
      llvm::sys::path::append(fasedCreditsPath, "post-fame-fased-ingress-credits.mlir");
      llvm::sys::path::append(fasedCreditsAnnotations, "post-fame-fased-ingress-credits-all.json");
      std::error_code fasedCreditsError;
      llvm::raw_fd_ostream fasedCreditsOut(fasedCreditsPath, fasedCreditsError);
      if (fasedCreditsError) return fail("cannot write FASED ingress credits: " + fasedCreditsError.message());
      module->print(fasedCreditsOut); fasedCreditsOut << '\n'; fasedCreditsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedCreditsAnnotations, error)))
        return fail("FASED ingress credit annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED ingress credits and completed-write detection in " << fasedCreditsPath << '\n';
      if (failed(goldengate::addFASEDIngressOrder(circuit, error)))
        return fail("FASED ingress order: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED ingress order produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedOrderPath(outputDir), fasedOrderAnnotations(outputDir);
      llvm::sys::path::append(fasedOrderPath, "post-fame-fased-ingress-order.mlir");
      llvm::sys::path::append(fasedOrderAnnotations, "post-fame-fased-ingress-order-all.json");
      std::error_code fasedOrderError;
      llvm::raw_fd_ostream fasedOrderOut(fasedOrderPath, fasedOrderError);
      if (fasedOrderError) return fail("cannot write FASED ingress order: " + fasedOrderError.message());
      module->print(fasedOrderOut); fasedOrderOut << '\n'; fasedOrderOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedOrderAnnotations, error)))
        return fail("FASED ingress order annotations: " + error);
      llvm::outs() << "Queued CIRCT FASED transaction order in two 10-entry banks in " << fasedOrderPath << '\n';
      if (failed(goldengate::addFASEDIngressIssue(circuit, error)))
        return fail("FASED ingress issue: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED ingress issue produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedIssuePath(outputDir), fasedIssueAnnotations(outputDir);
      llvm::sys::path::append(fasedIssuePath, "post-fame-fased-ingress-issue.mlir");
      llvm::sys::path::append(fasedIssueAnnotations, "post-fame-fased-ingress-issue-all.json");
      std::error_code fasedIssueError;
      llvm::raw_fd_ostream fasedIssueOut(fasedIssuePath, fasedIssueError);
      if (fasedIssueError) return fail("cannot write FASED ingress issue: " + fasedIssueError.message());
      module->print(fasedIssueOut); fasedIssueOut << '\n'; fasedIssueOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedIssueAnnotations, error)))
        return fail("FASED ingress issue annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host request issue and accepted order retirement in " << fasedIssuePath << '\n';
      if (failed(goldengate::addFASEDIngressDeadlock(circuit, error)))
        return fail("FASED ingress deadlock: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED ingress deadlock produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedDeadlockPath(outputDir), fasedDeadlockAnnotations(outputDir);
      llvm::sys::path::append(fasedDeadlockPath, "post-fame-fased-ingress-deadlock.mlir");
      llvm::sys::path::append(fasedDeadlockAnnotations, "post-fame-fased-ingress-deadlock-all.json");
      std::error_code fasedDeadlockError;
      llvm::raw_fd_ostream fasedDeadlockOut(fasedDeadlockPath, fasedDeadlockError);
      if (fasedDeadlockError) return fail("cannot write FASED ingress deadlock: " + fasedDeadlockError.message());
      module->print(fasedDeadlockOut); fasedDeadlockOut << '\n'; fasedDeadlockOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedDeadlockAnnotations, error)))
        return fail("FASED ingress deadlock annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED ingress enqueue deadlock assertions in " << fasedDeadlockPath << '\n';
      if (failed(goldengate::addFASEDReadBuffer(circuit, error)))
        return fail("FASED read buffer: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED read buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedReadBufferPath(outputDir), fasedReadBufferAnnotations(outputDir);
      llvm::sys::path::append(fasedReadBufferPath, "post-fame-fased-read-buffer.mlir");
      llvm::sys::path::append(fasedReadBufferAnnotations, "post-fame-fased-read-buffer-all.json");
      std::error_code fasedReadBufferError;
      llvm::raw_fd_ostream fasedReadBufferOut(fasedReadBufferPath, fasedReadBufferError);
      if (fasedReadBufferError) return fail("cannot write FASED read buffer: " + fasedReadBufferError.message());
      module->print(fasedReadBufferOut); fasedReadBufferOut << '\n'; fasedReadBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedReadBufferAnnotations, error)))
        return fail("FASED read buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT FASED read responses in sixteen 8-beat ID queues in " << fasedReadBufferPath << '\n';
      if (failed(goldengate::addFASEDReadScheduler(circuit, error)))
        return fail("FASED read scheduler: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED read scheduler produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedReadSchedulerPath(outputDir), fasedReadSchedulerAnnotations(outputDir);
      llvm::sys::path::append(fasedReadSchedulerPath, "post-fame-fased-read-scheduler.mlir");
      llvm::sys::path::append(fasedReadSchedulerAnnotations, "post-fame-fased-read-scheduler-all.json");
      std::error_code fasedReadSchedulerError;
      llvm::raw_fd_ostream fasedReadSchedulerOut(fasedReadSchedulerPath, fasedReadSchedulerError);
      if (fasedReadSchedulerError) return fail("cannot write FASED read scheduler: " + fasedReadSchedulerError.message());
      module->print(fasedReadSchedulerOut); fasedReadSchedulerOut << '\n'; fasedReadSchedulerOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedReadSchedulerAnnotations, error)))
        return fail("FASED read scheduler annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED read request scheduling and buffer retirement in " << fasedReadSchedulerPath << '\n';
      if (failed(goldengate::addFASEDWriteEgress(circuit, error)))
        return fail("FASED write egress: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED write egress produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWriteEgressPath(outputDir), fasedWriteEgressAnnotations(outputDir);
      llvm::sys::path::append(fasedWriteEgressPath, "post-fame-fased-write-egress.mlir");
      llvm::sys::path::append(fasedWriteEgressAnnotations, "post-fame-fased-write-egress-all.json");
      std::error_code fasedWriteEgressError;
      llvm::raw_fd_ostream fasedWriteEgressOut(fasedWriteEgressPath, fasedWriteEgressError);
      if (fasedWriteEgressError) return fail("cannot write FASED write egress: " + fasedWriteEgressError.message());
      module->print(fasedWriteEgressOut); fasedWriteEgressOut << '\n'; fasedWriteEgressOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWriteEgressAnnotations, error)))
        return fail("FASED write egress annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED per-ID write acknowledgements and token readiness in " << fasedWriteEgressPath << '\n';
      if (failed(goldengate::addFASEDResponseReleaser(circuit, error)))
        return fail("FASED response releaser: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED response releaser produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedResponseReleaserPath(outputDir), fasedResponseReleaserAnnotations(outputDir);
      llvm::sys::path::append(fasedResponseReleaserPath, "post-fame-fased-response-releaser.mlir");
      llvm::sys::path::append(fasedResponseReleaserAnnotations, "post-fame-fased-response-releaser-all.json");
      std::error_code fasedResponseReleaserError;
      llvm::raw_fd_ostream fasedResponseReleaserOut(fasedResponseReleaserPath, fasedResponseReleaserError);
      if (fasedResponseReleaserError) return fail("cannot write FASED response releaser: " + fasedResponseReleaserError.message());
      module->print(fasedResponseReleaserOut); fasedResponseReleaserOut << '\n'; fasedResponseReleaserOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedResponseReleaserAnnotations, error)))
        return fail("FASED response releaser annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED target response release and egress request binding in " << fasedResponseReleaserPath << '\n';
      if (failed(goldengate::addFASEDTimingCycle(circuit, error)))
        return fail("FASED timing cycle: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED timing cycle produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedTimingCyclePath(outputDir), fasedTimingCycleAnnotations(outputDir);
      llvm::sys::path::append(fasedTimingCyclePath, "post-fame-fased-timing-cycle.mlir");
      llvm::sys::path::append(fasedTimingCycleAnnotations, "post-fame-fased-timing-cycle-all.json");
      std::error_code fasedTimingCycleError;
      llvm::raw_fd_ostream fasedTimingCycleOut(fasedTimingCyclePath, fasedTimingCycleError);
      if (fasedTimingCycleError) return fail("cannot write FASED timing cycle: " + fasedTimingCycleError.message());
      module->print(fasedTimingCycleOut); fasedTimingCycleOut << '\n'; fasedTimingCycleOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedTimingCycleAnnotations, error)))
        return fail("FASED timing cycle annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED timing cycle and latency release-cycle arithmetic in " << fasedTimingCyclePath << '\n';
      if (failed(goldengate::addFASEDReadLatency(circuit, error)))
        return fail("FASED read latency: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED read latency produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedReadLatencyPath(outputDir), fasedReadLatencyAnnotations(outputDir);
      llvm::sys::path::append(fasedReadLatencyPath, "post-fame-fased-read-latency.mlir");
      llvm::sys::path::append(fasedReadLatencyAnnotations, "post-fame-fased-read-latency-all.json");
      std::error_code fasedReadLatencyError;
      llvm::raw_fd_ostream fasedReadLatencyOut(fasedReadLatencyPath, fasedReadLatencyError);
      if (fasedReadLatencyError) return fail("cannot write FASED read latency: " + fasedReadLatencyError.message());
      module->print(fasedReadLatencyOut); fasedReadLatencyOut << '\n'; fasedReadLatencyOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedReadLatencyAnnotations, error)))
        return fail("FASED read latency annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED ten-entry read latency queue and completion retirement in " << fasedReadLatencyPath << '\n';
      if (failed(goldengate::addFASEDWriteLatency(circuit, error)))
        return fail("FASED write latency: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED write latency produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWriteLatencyPath(outputDir), fasedWriteLatencyAnnotations(outputDir);
      llvm::sys::path::append(fasedWriteLatencyPath, "post-fame-fased-write-latency.mlir");
      llvm::sys::path::append(fasedWriteLatencyAnnotations, "post-fame-fased-write-latency-all.json");
      std::error_code fasedWriteLatencyError;
      llvm::raw_fd_ostream fasedWriteLatencyOut(fasedWriteLatencyPath, fasedWriteLatencyError);
      if (fasedWriteLatencyError) return fail("cannot write FASED write latency: " + fasedWriteLatencyError.message());
      module->print(fasedWriteLatencyOut); fasedWriteLatencyOut << '\n'; fasedWriteLatencyOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWriteLatencyAnnotations, error)))
        return fail("FASED write latency annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED ten-entry write latency queue and completion boundary in " << fasedWriteLatencyPath << '\n';
      if (failed(goldengate::addFASEDTimingAWQueue(circuit, error)))
        return fail("FASED timing AW queue: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED timing AW queue produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedTimingAWQueuePath(outputDir), fasedTimingAWQueueAnnotations(outputDir);
      llvm::sys::path::append(fasedTimingAWQueuePath, "post-fame-fased-timing-aw-queue.mlir");
      llvm::sys::path::append(fasedTimingAWQueueAnnotations, "post-fame-fased-timing-aw-queue-all.json");
      std::error_code fasedTimingAWQueueError;
      llvm::raw_fd_ostream fasedTimingAWQueueOut(fasedTimingAWQueuePath, fasedTimingAWQueueError);
      if (fasedTimingAWQueueError) return fail("cannot write FASED timing AW queue: " + fasedTimingAWQueueError.message());
      module->print(fasedTimingAWQueueOut); fasedTimingAWQueueOut << '\n'; fasedTimingAWQueueOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedTimingAWQueueAnnotations, error)))
        return fail("FASED timing AW queue annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED ten-entry timing AW metadata queue and completion ID binding in " << fasedTimingAWQueuePath << '\n';
      if (failed(goldengate::addFASEDWritePairing(circuit, error)))
        return fail("FASED write pairing: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED write pairing produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWritePairingPath(outputDir), fasedWritePairingAnnotations(outputDir);
      llvm::sys::path::append(fasedWritePairingPath, "post-fame-fased-write-pairing.mlir");
      llvm::sys::path::append(fasedWritePairingAnnotations, "post-fame-fased-write-pairing-all.json");
      std::error_code fasedWritePairingError;
      llvm::raw_fd_ostream fasedWritePairingOut(fasedWritePairingPath, fasedWritePairingError);
      if (fasedWritePairingError) return fail("cannot write FASED write pairing: " + fasedWritePairingError.message());
      module->print(fasedWritePairingOut); fasedWritePairingOut << '\n'; fasedWritePairingOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWritePairingAnnotations, error)))
        return fail("FASED write pairing annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED pending AW/W counters and completion pairing in " << fasedWritePairingPath << '\n';
      if (failed(goldengate::bindFASEDWriteRetirement(circuit, error)))
        return fail("FASED write retirement: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED write retirement produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWriteRetirementPath(outputDir), fasedWriteRetirementAnnotations(outputDir);
      llvm::sys::path::append(fasedWriteRetirementPath, "post-fame-fased-write-retirement.mlir");
      llvm::sys::path::append(fasedWriteRetirementAnnotations, "post-fame-fased-write-retirement-all.json");
      std::error_code fasedWriteRetirementError;
      llvm::raw_fd_ostream fasedWriteRetirementOut(fasedWriteRetirementPath, fasedWriteRetirementError);
      if (fasedWriteRetirementError) return fail("cannot write FASED write retirement: " + fasedWriteRetirementError.message());
      module->print(fasedWriteRetirementOut); fasedWriteRetirementOut << '\n'; fasedWriteRetirementOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWriteRetirementAnnotations, error)))
        return fail("FASED write retirement annotations: " + error);
      llvm::outs() << "Bound CIRCT FASED accepted target B responses to pending write counters in " << fasedWriteRetirementPath << '\n';
      if (failed(goldengate::bindFASEDWriteAdmission(circuit, error)))
        return fail("FASED write admission: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED write admission produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedWriteAdmissionPath(outputDir), fasedWriteAdmissionAnnotations(outputDir);
      llvm::sys::path::append(fasedWriteAdmissionPath, "post-fame-fased-write-admission.mlir");
      llvm::sys::path::append(fasedWriteAdmissionAnnotations, "post-fame-fased-write-admission-all.json");
      std::error_code fasedWriteAdmissionError;
      llvm::raw_fd_ostream fasedWriteAdmissionOut(fasedWriteAdmissionPath, fasedWriteAdmissionError);
      if (fasedWriteAdmissionError) return fail("cannot write FASED write admission: " + fasedWriteAdmissionError.message());
      module->print(fasedWriteAdmissionOut); fasedWriteAdmissionOut << '\n'; fasedWriteAdmissionOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedWriteAdmissionAnnotations, error)))
        return fail("FASED write admission annotations: " + error);
      llvm::outs() << "Bound CIRCT FASED target AW/W readiness to pending write counters in " << fasedWriteAdmissionPath << '\n';
      if (failed(goldengate::bindFASEDReadAdmission(circuit, error)))
        return fail("FASED read admission: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED read admission produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedReadAdmissionPath(outputDir), fasedReadAdmissionAnnotations(outputDir);
      llvm::sys::path::append(fasedReadAdmissionPath, "post-fame-fased-read-admission.mlir");
      llvm::sys::path::append(fasedReadAdmissionAnnotations, "post-fame-fased-read-admission-all.json");
      std::error_code fasedReadAdmissionError;
      llvm::raw_fd_ostream fasedReadAdmissionOut(fasedReadAdmissionPath, fasedReadAdmissionError);
      if (fasedReadAdmissionError) return fail("cannot write FASED read admission: " + fasedReadAdmissionError.message());
      module->print(fasedReadAdmissionOut); fasedReadAdmissionOut << '\n'; fasedReadAdmissionOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedReadAdmissionAnnotations, error)))
        return fail("FASED read admission annotations: " + error);
      llvm::outs() << "Bound CIRCT FASED pending reads, final R retirement and AR admission in " << fasedReadAdmissionPath << '\n';
      if (failed(goldengate::addFASEDRequestLimits(circuit, error)))
        return fail("FASED request limits: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED request limits produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedRequestLimitsPath(outputDir), fasedRequestLimitsAnnotations(outputDir);
      llvm::sys::path::append(fasedRequestLimitsPath, "post-fame-fased-request-limits.mlir");
      llvm::sys::path::append(fasedRequestLimitsAnnotations, "post-fame-fased-request-limits-all.json");
      std::error_code fasedRequestLimitsError;
      llvm::raw_fd_ostream fasedRequestLimitsOut(fasedRequestLimitsPath, fasedRequestLimitsError);
      if (fasedRequestLimitsError) return fail("cannot write FASED request limits: " + fasedRequestLimitsError.message());
      module->print(fasedRequestLimitsOut); fasedRequestLimitsOut << '\n'; fasedRequestLimitsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedRequestLimitsAnnotations, error)))
        return fail("FASED request limits annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host-clock request-limit registers and admission binding in " << fasedRequestLimitsPath << '\n';
      if (failed(goldengate::addFASEDLatencyRegisters(circuit, error)))
        return fail("FASED latency registers: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED latency registers produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedLatencyRegistersPath(outputDir), fasedLatencyRegistersAnnotations(outputDir);
      llvm::sys::path::append(fasedLatencyRegistersPath, "post-fame-fased-latency-registers.mlir");
      llvm::sys::path::append(fasedLatencyRegistersAnnotations, "post-fame-fased-latency-registers-all.json");
      std::error_code fasedLatencyRegistersError;
      llvm::raw_fd_ostream fasedLatencyRegistersOut(fasedLatencyRegistersPath, fasedLatencyRegistersError);
      if (fasedLatencyRegistersError) return fail("cannot write FASED latency registers: " + fasedLatencyRegistersError.message());
      module->print(fasedLatencyRegistersOut); fasedLatencyRegistersOut << '\n'; fasedLatencyRegistersOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedLatencyRegistersAnnotations, error)))
        return fail("FASED latency registers annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host-clock latency registers and timing binding in " << fasedLatencyRegistersPath << '\n';
      if (failed(goldengate::addFASEDFunctionalModelRegister(circuit, error)))
        return fail("FASED functional model register: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED functional model register produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedFunctionalModelRegisterPath(outputDir), fasedFunctionalModelRegisterAnnotations(outputDir);
      llvm::sys::path::append(fasedFunctionalModelRegisterPath, "post-fame-fased-functional-model-register.mlir");
      llvm::sys::path::append(fasedFunctionalModelRegisterAnnotations, "post-fame-fased-functional-model-register-all.json");
      std::error_code fasedFunctionalModelRegisterError;
      llvm::raw_fd_ostream fasedFunctionalModelRegisterOut(fasedFunctionalModelRegisterPath, fasedFunctionalModelRegisterError);
      if (fasedFunctionalModelRegisterError) return fail("cannot write FASED functional model register: " + fasedFunctionalModelRegisterError.message());
      module->print(fasedFunctionalModelRegisterOut); fasedFunctionalModelRegisterOut << '\n'; fasedFunctionalModelRegisterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedFunctionalModelRegisterAnnotations, error)))
        return fail("FASED functional model register annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host-clock functional model register and ingress binding in " << fasedFunctionalModelRegisterPath << '\n';
      if (failed(goldengate::addFASEDResponseErrors(circuit, error)))
        return fail("FASED response error registers: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED response error registers produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedResponseErrorsPath(outputDir), fasedResponseErrorsAnnotations(outputDir);
      llvm::sys::path::append(fasedResponseErrorsPath, "post-fame-fased-response-errors.mlir");
      llvm::sys::path::append(fasedResponseErrorsAnnotations, "post-fame-fased-response-errors-all.json");
      std::error_code fasedResponseErrorsError;
      llvm::raw_fd_ostream fasedResponseErrorsOut(fasedResponseErrorsPath, fasedResponseErrorsError);
      if (fasedResponseErrorsError) return fail("cannot write FASED response error registers: " + fasedResponseErrorsError.message());
      module->print(fasedResponseErrorsOut); fasedResponseErrorsOut << '\n'; fasedResponseErrorsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedResponseErrorsAnnotations, error)))
        return fail("FASED response error registers annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED host-clock response error registers and response handshake binding in " << fasedResponseErrorsPath << '\n';
      if (failed(goldengate::addFASEDStatistics(circuit, error)))
        return fail("FASED transaction and beat counters: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED transaction and beat counters produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedStatisticsPath(outputDir), fasedStatisticsAnnotations(outputDir);
      llvm::sys::path::append(fasedStatisticsPath, "post-fame-fased-statistics.mlir");
      llvm::sys::path::append(fasedStatisticsAnnotations, "post-fame-fased-statistics-all.json");
      std::error_code fasedStatisticsError;
      llvm::raw_fd_ostream fasedStatisticsOut(fasedStatisticsPath, fasedStatisticsError);
      if (fasedStatisticsError) return fail("cannot write FASED transaction and beat counters: " + fasedStatisticsError.message());
      module->print(fasedStatisticsOut); fasedStatisticsOut << '\n'; fasedStatisticsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedStatisticsAnnotations, error)))
        return fail("FASED transaction and beat counters annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED target-clock transaction and beat counters and read-only MMIO in " << fasedStatisticsPath << '\n';
      if (failed(goldengate::addFASEDHistograms(circuit, error)))
        return fail("FASED outstanding occupancy histograms: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED outstanding occupancy histograms produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedHistogramsPath(outputDir), fasedHistogramsAnnotations(outputDir);
      llvm::sys::path::append(fasedHistogramsPath, "post-fame-fased-histograms.mlir");
      llvm::sys::path::append(fasedHistogramsAnnotations, "post-fame-fased-histograms-all.json");
      std::error_code fasedHistogramsError;
      llvm::raw_fd_ostream fasedHistogramsOut(fasedHistogramsPath, fasedHistogramsError);
      if (fasedHistogramsError) return fail("cannot write FASED outstanding occupancy histograms: " + fasedHistogramsError.message());
      module->print(fasedHistogramsOut); fasedHistogramsOut << '\n'; fasedHistogramsOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedHistogramsAnnotations, error)))
        return fail("FASED outstanding occupancy histograms annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED target-clock occupancy histograms and read-only MMIO in " << fasedHistogramsPath << '\n';
      if (failed(goldengate::addFASEDMMIOBank(circuit, error)))
        return fail("FASED MMIO bank: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED MMIO bank produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedMMIOPath(outputDir), fasedMMIOAnnotations(outputDir);
      llvm::sys::path::append(fasedMMIOPath, "post-fame-fased-mmio.mlir");
      llvm::sys::path::append(fasedMMIOAnnotations, "post-fame-fased-mmio-all.json");
      std::error_code fasedMMIOError;
      llvm::raw_fd_ostream fasedMMIOOut(fasedMMIOPath, fasedMMIOError);
      if (fasedMMIOError) return fail("cannot write FASED MMIO bank: " + fasedMMIOError.message());
      module->print(fasedMMIOOut); fasedMMIOOut << '\n'; fasedMMIOOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedMMIOAnnotations, error)))
        return fail("FASED MMIO bank annotations: " + error);
      llvm::outs() << "Assembled CIRCT FASED 21-word MMIO bank in " << fasedMMIOPath << '\n';
      if (failed(goldengate::mapFASEDBridgeControl(circuit, 25, 12, error)))
        return fail("FASED MCRFile control: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED MCRFile control produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedControlPath(outputDir), fasedControlAnnotations(outputDir);
      llvm::sys::path::append(fasedControlPath, "post-fame-fased-control.mlir");
      llvm::sys::path::append(fasedControlAnnotations, "post-fame-fased-control-all.json");
      std::error_code fasedControlError;
      llvm::raw_fd_ostream fasedControlOut(fasedControlPath, fasedControlError);
      if (fasedControlError) return fail("cannot write FASED MCRFile control: " + fasedControlError.message());
      module->print(fasedControlOut); fasedControlOut << '\n'; fasedControlOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedControlAnnotations, error)))
        return fail("FASED MCRFile control annotations: " + error);
      llvm::outs() << "Mapped CIRCT FASED MCRFile transactions in " << fasedControlPath << '\n';
      if (failed(goldengate::bindFASEDBridgeControl(circuit, error)))
        return fail("FASED control slave binding: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED control slave binding produced invalid FIRRTL IR");
      llvm::SmallString<256> fasedBoundPath(outputDir), fasedBoundAnnotations(outputDir);
      llvm::sys::path::append(fasedBoundPath, "post-fame-fased-bound.mlir");
      llvm::sys::path::append(fasedBoundAnnotations, "post-fame-fased-bound-all.json");
      std::error_code fasedBoundError;
      llvm::raw_fd_ostream fasedBoundOut(fasedBoundPath, fasedBoundError);
      if (fasedBoundError) return fail("cannot write FASED control binding: " + fasedBoundError.message());
      module->print(fasedBoundOut); fasedBoundOut << '\n'; fasedBoundOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fasedBoundAnnotations, error)))
        return fail("FASED control binding annotations: " + error);
      llvm::outs() << "Bound CIRCT FASED requests and responses to control slave 1 in " << fasedBoundPath << '\n';
      if (failed(goldengate::bindControlMaster(circuit, error)))
        return fail("host control master binding: " + error);
      if (failed(mlir::verify(*module))) return fail("host control master binding produced invalid FIRRTL IR");
      llvm::SmallString<256> controlMasterPath(outputDir), controlMasterAnnotations(outputDir);
      llvm::sys::path::append(controlMasterPath, "post-fame-control-master.mlir");
      llvm::sys::path::append(controlMasterAnnotations, "post-fame-control-master-all.json");
      std::error_code controlMasterError;
      llvm::raw_fd_ostream controlMasterOut(controlMasterPath, controlMasterError);
      if (controlMasterError) return fail("cannot write host control master binding: " + controlMasterError.message());
      module->print(controlMasterOut); controlMasterOut << '\n'; controlMasterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, controlMasterAnnotations, error)))
        return fail("host control master annotations: " + error);
      llvm::outs() << "Assembled CIRCT host control master in " << controlMasterPath << '\n';
      if (failed(goldengate::bindFASEDHostMemory(circuit, error)))
        return fail("FASED host memory binding: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED host memory binding produced invalid FIRRTL IR");
      llvm::SmallString<256> hostMemoryPath(outputDir), hostMemoryAnnotations(outputDir);
      llvm::sys::path::append(hostMemoryPath, "post-fame-fased-host-memory.mlir");
      llvm::sys::path::append(hostMemoryAnnotations, "post-fame-fased-host-memory-all.json");
      std::error_code hostMemoryError;
      llvm::raw_fd_ostream hostMemoryOut(hostMemoryPath, hostMemoryError);
      if (hostMemoryError) return fail("cannot write FASED host memory binding: " + hostMemoryError.message());
      module->print(hostMemoryOut); hostMemoryOut << '\n'; hostMemoryOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostMemoryAnnotations, error)))
        return fail("FASED host memory annotations: " + error);
      llvm::outs() << "Assembled CIRCT FASED host memory master in " << hostMemoryPath << '\n';
      if (failed(goldengate::addFASEDAddressTranslation(circuit, error)))
        return fail("FASED address translation: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED address translation produced invalid FIRRTL IR");
      llvm::SmallString<256> translationPath(outputDir), translationAnnotations(outputDir);
      llvm::sys::path::append(translationPath, "post-fame-fased-address-translation.mlir");
      llvm::sys::path::append(translationAnnotations, "post-fame-fased-address-translation-all.json");
      std::error_code translationError;
      llvm::raw_fd_ostream translationOut(translationPath, translationError);
      if (translationError) return fail("cannot write FASED address translation: " + translationError.message());
      module->print(translationOut); translationOut << '\n'; translationOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, translationAnnotations, error)))
        return fail("FASED address translation annotations: " + error);
      llvm::outs() << "Translated CIRCT FASED addresses and host-clock bounds assertions in " << translationPath << '\n';
      if (failed(goldengate::addFASEDReadDeinterleaver(circuit, error)))
        return fail("FASED read deinterleaver: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED read deinterleaver produced invalid FIRRTL IR");
      llvm::SmallString<256> deinterleavePath(outputDir), deinterleaveAnnotations(outputDir);
      llvm::sys::path::append(deinterleavePath, "post-fame-fased-read-deinterleaver.mlir");
      llvm::sys::path::append(deinterleaveAnnotations, "post-fame-fased-read-deinterleaver-all.json");
      std::error_code deinterleaveError;
      llvm::raw_fd_ostream deinterleaveOut(deinterleavePath, deinterleaveError);
      if (deinterleaveError) return fail("cannot write FASED read deinterleaver: " + deinterleaveError.message());
      module->print(deinterleaveOut); deinterleaveOut << '\n'; deinterleaveOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, deinterleaveAnnotations, error)))
        return fail("FASED read deinterleaver annotations: " + error);
      llvm::outs() << "Deinterleaved CIRCT FASED responses with sixteen eight-beat queues in " << deinterleavePath << '\n';
      if (failed(goldengate::addFASEDHostMemoryBuffer(circuit, error)))
        return fail("FASED host memory buffer: " + error);
      if (failed(mlir::verify(*module))) return fail("FASED host memory buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> memoryBufferPath(outputDir), memoryBufferAnnotations(outputDir);
      llvm::sys::path::append(memoryBufferPath, "post-fame-fased-host-memory-buffer.mlir");
      llvm::sys::path::append(memoryBufferAnnotations, "post-fame-fased-host-memory-buffer-all.json");
      std::error_code memoryBufferError;
      llvm::raw_fd_ostream memoryBufferOut(memoryBufferPath, memoryBufferError);
      if (memoryBufferError) return fail("cannot write FASED host memory buffer: " + memoryBufferError.message());
      module->print(memoryBufferOut); memoryBufferOut << '\n'; memoryBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, memoryBufferAnnotations, error)))
        return fail("FASED host memory buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT FASED host memory with five two-entry queues in " << memoryBufferPath << '\n';
      if (failed(goldengate::addHostMemoryWriteArbiter(circuit, error)))
        return fail("host memory write arbiter: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory write arbiter produced invalid FIRRTL IR");
      llvm::SmallString<256> hostMemoryWriteArbiterPath(outputDir), hostMemoryWriteArbiterAnnotations(outputDir);
      llvm::sys::path::append(hostMemoryWriteArbiterPath, "post-fame-host-memory-write-arbiter.mlir");
      llvm::sys::path::append(hostMemoryWriteArbiterAnnotations, "post-fame-host-memory-write-arbiter-all.json");
      std::error_code hostMemoryWriteArbiterError;
      llvm::raw_fd_ostream hostMemoryWriteArbiterOut(hostMemoryWriteArbiterPath, hostMemoryWriteArbiterError);
      if (hostMemoryWriteArbiterError) return fail("cannot write host memory write arbiter: " + hostMemoryWriteArbiterError.message());
      module->print(hostMemoryWriteArbiterOut); hostMemoryWriteArbiterOut << '\n'; hostMemoryWriteArbiterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostMemoryWriteArbiterAnnotations, error)))
        return fail("host memory write arbiter annotations: " + error);
      llvm::outs() << "Arbitrated CIRCT LoadMem and FASED AW/W with a two-entry source queue in " << hostMemoryWriteArbiterPath << '\n';
      if (failed(goldengate::routeHostMemoryWriteResponses(circuit, error)))
        return fail("host memory write responses: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory write responses produced invalid FIRRTL IR");
      llvm::SmallString<256> hostWriteResponsePath(outputDir), hostWriteResponseAnnotations(outputDir);
      llvm::sys::path::append(hostWriteResponsePath, "post-fame-host-memory-write-responses.mlir");
      llvm::sys::path::append(hostWriteResponseAnnotations, "post-fame-host-memory-write-responses-all.json");
      std::error_code hostWriteResponseError;
      llvm::raw_fd_ostream hostWriteResponseOut(hostWriteResponsePath, hostWriteResponseError);
      if (hostWriteResponseError) return fail("cannot write host memory write responses: " + hostWriteResponseError.message());
      module->print(hostWriteResponseOut); hostWriteResponseOut << '\n'; hostWriteResponseOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostWriteResponseAnnotations, error)))
        return fail("host memory write response annotations: " + error);
      llvm::outs() << "Routed CIRCT host B responses to LoadMem ID 16 and FASED IDs 0..15 in " << hostWriteResponsePath << '\n';
      if (failed(goldengate::addHostMemoryReadArbiter(circuit, error)))
        return fail("host memory read arbiter: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory read arbiter produced invalid FIRRTL IR");
      llvm::SmallString<256> hostReadArbiterPath(outputDir), hostReadArbiterAnnotations(outputDir);
      llvm::sys::path::append(hostReadArbiterPath, "post-fame-host-memory-read-arbiter.mlir");
      llvm::sys::path::append(hostReadArbiterAnnotations, "post-fame-host-memory-read-arbiter-all.json");
      std::error_code hostReadArbiterError;
      llvm::raw_fd_ostream hostReadArbiterOut(hostReadArbiterPath, hostReadArbiterError);
      if (hostReadArbiterError) return fail("cannot write host memory read arbiter: " + hostReadArbiterError.message());
      module->print(hostReadArbiterOut); hostReadArbiterOut << '\n'; hostReadArbiterOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostReadArbiterAnnotations, error)))
        return fail("host memory read arbiter annotations: " + error);
      llvm::outs() << "Arbitrated CIRCT LoadMem and FASED AR requests in " << hostReadArbiterPath << '\n';
      if (failed(goldengate::routeHostMemoryReadResponses(circuit, error)))
        return fail("host memory read responses: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory read responses produced invalid FIRRTL IR");
      llvm::SmallString<256> hostReadResponsePath(outputDir), hostReadResponseAnnotations(outputDir);
      llvm::sys::path::append(hostReadResponsePath, "post-fame-host-memory-read-responses.mlir");
      llvm::sys::path::append(hostReadResponseAnnotations, "post-fame-host-memory-read-responses-all.json");
      std::error_code hostReadResponseError;
      llvm::raw_fd_ostream hostReadResponseOut(hostReadResponsePath, hostReadResponseError);
      if (hostReadResponseError) return fail("cannot write host memory read responses: " + hostReadResponseError.message());
      module->print(hostReadResponseOut); hostReadResponseOut << '\n'; hostReadResponseOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostReadResponseAnnotations, error)))
        return fail("host memory read response annotations: " + error);
      llvm::outs() << "Routed CIRCT host R responses to LoadMem ID16 and FASED IDs0..15 in " << hostReadResponsePath << '\n';
      if (failed(goldengate::addHostMemoryOutputBuffer(circuit, error)))
        return fail("host memory output buffer: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory output buffer produced invalid FIRRTL IR");
      llvm::SmallString<256> hostOutputBufferPath(outputDir), hostOutputBufferAnnotations(outputDir);
      llvm::sys::path::append(hostOutputBufferPath, "post-fame-host-memory-output-buffer.mlir");
      llvm::sys::path::append(hostOutputBufferAnnotations, "post-fame-host-memory-output-buffer-all.json");
      std::error_code hostOutputBufferError;
      llvm::raw_fd_ostream hostOutputBufferOut(hostOutputBufferPath, hostOutputBufferError);
      if (hostOutputBufferError) return fail("cannot write host memory output buffer: " + hostOutputBufferError.message());
      module->print(hostOutputBufferOut); hostOutputBufferOut << '\n'; hostOutputBufferOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostOutputBufferAnnotations, error)))
        return fail("host memory output buffer annotations: " + error);
      llvm::outs() << "Buffered CIRCT host memory output with five two-entry queues in " << hostOutputBufferPath << '\n';
      if (failed(goldengate::specializeHostMemoryPort(circuit, error)))
        return fail("host memory platform port: " + error);
      if (failed(mlir::verify(*module))) return fail("host memory platform port produced invalid FIRRTL IR");
      llvm::SmallString<256> hostPlatformPath(outputDir), hostPlatformAnnotations(outputDir);
      llvm::sys::path::append(hostPlatformPath, "post-fame-host-memory-platform.mlir");
      llvm::sys::path::append(hostPlatformAnnotations, "post-fame-host-memory-platform-all.json");
      std::error_code hostPlatformError;
      llvm::raw_fd_ostream hostPlatformOut(hostPlatformPath, hostPlatformError);
      if (hostPlatformError) return fail("cannot write host memory platform port: " + hostPlatformError.message());
      module->print(hostPlatformOut); hostPlatformOut << '\n'; hostPlatformOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, hostPlatformAnnotations, error)))
        return fail("host memory platform annotations: " + error);
      llvm::outs() << "Adapted CIRCT host memory to the sixteen-bit mem_0 ID interface in " << hostPlatformPath << '\n';
      if (failed(goldengate::assembleCPUStreamPort(circuit, error)))
        return fail("CPU stream platform port: " + error);
      if (failed(mlir::verify(*module))) return fail("CPU stream platform port produced invalid FIRRTL IR");
      llvm::SmallString<256> cpuPlatformPath(outputDir), cpuPlatformAnnotations(outputDir);
      llvm::sys::path::append(cpuPlatformPath, "post-fame-cpu-stream-platform.mlir");
      llvm::sys::path::append(cpuPlatformAnnotations, "post-fame-cpu-stream-platform-all.json");
      std::error_code cpuPlatformError;
      llvm::raw_fd_ostream cpuPlatformOut(cpuPlatformPath, cpuPlatformError);
      if (cpuPlatformError) return fail("cannot write CPU stream platform port: " + cpuPlatformError.message());
      module->print(cpuPlatformOut); cpuPlatformOut << '\n'; cpuPlatformOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, cpuPlatformAnnotations, error)))
        return fail("CPU stream platform annotations: " + error);
      llvm::outs() << "Assembled CIRCT CPU-managed AXI4 slave in " << cpuPlatformPath << '\n';
      if (failed(goldengate::assembleFPGATopShell(circuit, error)))
        return fail("FPGA top shell: " + error);
      if (failed(mlir::verify(*module))) return fail("FPGA top shell produced invalid FIRRTL IR");
      llvm::SmallString<256> fpgaTopPath(outputDir), fpgaTopAnnotations(outputDir);
      llvm::sys::path::append(fpgaTopPath, "post-fame-fpga-top.mlir");
      llvm::sys::path::append(fpgaTopAnnotations, "post-fame-fpga-top-all.json");
      std::error_code fpgaTopError;
      llvm::raw_fd_ostream fpgaTopOut(fpgaTopPath, fpgaTopError);
      if (fpgaTopError) return fail("cannot write FPGA top shell: " + fpgaTopError.message());
      module->print(fpgaTopOut); fpgaTopOut << '\n'; fpgaTopOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, fpgaTopAnnotations, error)))
        return fail("FPGA top shell annotations: " + error);
      llvm::outs() << "Assembled CIRCT FPGATop clock/reset and three AXI4 interfaces in " << fpgaTopPath << '\n';
      if (failed(goldengate::assembleF1Shim(circuit, error)))
        return fail("U250 F1 shim: " + error);
      if (failed(mlir::verify(*module))) return fail("U250 F1 shim produced invalid FIRRTL IR");
      llvm::SmallString<256> shimPath(outputDir), shimAnnotations(outputDir);
      llvm::sys::path::append(shimPath, "post-fame-f1-shim.mlir");
      llvm::sys::path::append(shimAnnotations, "post-fame-f1-shim-all.json");
      std::error_code shimError;
      llvm::raw_fd_ostream shimOut(shimPath, shimError);
      if (shimError) return fail("cannot write F1 shim: " + shimError.message());
      module->print(shimOut); shimOut << '\n'; shimOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, shimAnnotations, error)))
        return fail("F1 shim annotations: " + error);
      llvm::outs() << "Assembled CIRCT U250 F1Shim and accepted control request ID counters in " << shimPath << '\n';
      if (failed(goldengate::specializeXilinxClockGates(circuit, error)))
        return fail("Xilinx host specialization: " + error);
      if (failed(mlir::verify(*module)))
        return fail("Xilinx host specialization produced invalid FIRRTL IR");
      llvm::SmallString<256> xilinxPath(outputDir), xilinxAnnotations(outputDir);
      llvm::sys::path::append(xilinxPath, "post-xilinx-host-specialization.mlir");
      llvm::sys::path::append(xilinxAnnotations, "post-xilinx-host-specialization-all.json");
      std::error_code xilinxError;
      llvm::raw_fd_ostream xilinxOut(xilinxPath, xilinxError);
      if (xilinxError)
        return fail("cannot write Xilinx host specialization: " + xilinxError.message());
      module->print(xilinxOut); xilinxOut << '\n'; xilinxOut.close();
      if (failed(goldengate::emitAllAnnotations(circuit, xilinxAnnotations, error)))
        return fail("Xilinx host specialization annotations: " + error);
      llvm::outs() << "Specialized CIRCT abstract clocks to Xilinx BUFGCE in " << xilinxPath << '\n';
      if (failed(goldengate::prepareXDCOutput(circuit, error)))
        return fail("XDC output preparation: " + error);
      if (failed(goldengate::prepareMetasimInterfaceHeader(circuit, originalTargetName, error)))
        return fail("metasim interface header: " + error);
      if (failed(goldengate::prepareSimulationMasterHeader(circuit, error)))
        return fail("SimulationMaster driver header: " + error);
      if (failed(goldengate::prepareClockBridgeHeader(circuit, error)))
        return fail("ClockBridge driver header: " + error);
      if (failed(goldengate::prepareResetPulseHeader(circuit, error)))
        return fail("ResetPulseBridge driver header: " + error);
      if (failed(goldengate::prepareLoadMemHeader(circuit, error)))
        return fail("LoadMem driver header: " + error);
      if (failed(goldengate::preparePeekPokeHeader(circuit, error)))
        return fail("PeekPoke driver header: " + error);
      if (failed(goldengate::prepareUARTHeader(circuit, error)))
        return fail("UART driver header: " + error);
      if (failed(goldengate::prepareTSIHeader(circuit, error)))
        return fail("TSI driver header: " + error);
      if (failed(goldengate::prepareBlockDevHeader(circuit, error)))
        return fail("BlockDev driver header: " + error);
      if (failed(goldengate::prepareTracerVHeader(circuit, error)))
        return fail("TracerV driver header: " + error);
      if (failed(goldengate::prepareCPUManagedStreamHeader(circuit, error)))
        return fail("CPU managed stream driver header: " + error);
      if (failed(goldengate::prepareFASEDHeader(circuit, error)))
        return fail("FASED driver header: " + error);
      llvm::SmallString<256> xdcAnnotations(outputDir);
      llvm::sys::path::append(xdcAnnotations, "post-fame-xdc-all.json");
      if (failed(goldengate::emitAllAnnotations(circuit, xdcAnnotations, error)))
        return fail("XDC output annotations: " + error);
      if (failed(goldengate::emitOutputFiles(circuit, outputDir, outputBase, error)))
        return fail("Golden Gate output files: " + error);
      llvm::outs() << "Emitted CIRCT Golden Gate annotated collateral with base " << outputBase << '\n';
      llvm::SmallString<256> rtlPath(outputDir);
      llvm::sys::path::append(rtlPath, outputBase + ".sv");
      if (failed(goldengate::emitSimulatorRTL(*module, firPath, rtlPath, error)))
        return fail("simulator RTL: " + error);
      llvm::outs() << "Emitted CIRCT simulator RTL in " << rtlPath << '\n';
    }
    return 0;
  }

  auto emitPrintStubBoundary = [&]() -> int {
    std::string error;
    llvm::SmallVector<goldengate::PrintStub> stubs;
    if (failed(goldengate::synthesizePrintStubs(circuit, stubs, error)))
      return fail("PrintSynthesis stubs: " + error);
    if (failed(mlir::verify(*module)))
      return fail("PrintSynthesis stubs produced invalid FIRRTL IR");
    llvm::json::Array summary;
    for (auto &stub : stubs) {
      llvm::json::Array fields;
      for (auto field : cast<BundleType>(stub.bundle.getResult().getType()).getElements()) {
        std::string type;
        llvm::raw_string_ostream text(type);
        field.type.print(text);
        fields.push_back(llvm::json::Object{{"name", field.name.getValue().str()}, {"type", type}});
      }
      summary.push_back(llvm::json::Object{{"target", stub.target},
          {"clock", stub.clockTarget}, {"format", stub.formatString}, {"fields", std::move(fields)}});
    }
    llvm::SmallString<256> irPath(outputDir), annoPath(outputDir), summaryPath(outputDir);
    llvm::sys::path::append(irPath, "post-print-stubs.mlir");
    llvm::sys::path::append(annoPath, "post-print-stubs-all.json");
    llvm::sys::path::append(summaryPath, "print-stubs.json");
    std::error_code ec;
    llvm::raw_fd_ostream ir(irPath, ec);
    if (ec) return fail("cannot write print stub IR: " + ec.message());
    module->print(ir); ir << '\n';
    llvm::raw_fd_ostream metadata(summaryPath, ec);
    if (ec) return fail("cannot write print stub summary: " + ec.message());
    metadata << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(summary)));
    if (failed(goldengate::emitAllAnnotations(circuit, annoPath, error)))
      return fail("cannot export print stub annotations: " + error);
    llvm::outs() << "Synthesized " << stubs.size() << " printf bundles in " << irPath << '\n';
    if (wireAutoCounterStubs || wirePrintStubs) {
      llvm::SmallVector<goldengate::WiredPrint> routes;
      if (failed(goldengate::wirePrintStubsToTop(circuit, stubs, routes, error)))
        return fail("PrintSynthesis bundle top wiring: " + error);
      if (failed(mlir::verify(*module)))
        return fail("PrintSynthesis bundle top wiring produced invalid FIRRTL IR");
      llvm::SmallString<256> wiredIR(outputDir), wiredAnnos(outputDir), wiredSummary(outputDir);
      llvm::sys::path::append(wiredIR, "post-print-wiring.mlir");
      llvm::sys::path::append(wiredAnnos, "post-print-wiring-all.json");
      llvm::sys::path::append(wiredSummary, "print-wiring.json");
      llvm::raw_fd_ostream wired(wiredIR, ec);
      if (ec) return fail("cannot write print wiring IR: " + ec.message());
      module->print(wired); wired << '\n';
      llvm::json::Array routing;
      for (auto &route : routes)
        routing.push_back(llvm::json::Object{{"source", stubs[route.stubIndex].target},
            {"absoluteSource", route.absoluteSource}, {"topTarget", route.topTarget},
            {"depth", int64_t(route.instancePath.size())}});
      llvm::raw_fd_ostream metadata(wiredSummary, ec);
      if (ec) return fail("cannot write print wiring summary: " + ec.message());
      metadata << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(routing)));
      if (failed(goldengate::emitAllAnnotations(circuit, wiredAnnos, error)))
        return fail("cannot export pending print wiring annotations: " + error);
      llvm::outs() << "Routed " << routes.size() << " printf bundles to top; clock binding pending\n";
      if (analyzeAutoCounterPrintClocks) {
        llvm::SmallVector<goldengate::PrintClockSource> sources;
        if (failed(goldengate::analyzePrintClockSources(circuit, stubs, routes, sources, error)))
          return fail("PrintSynthesis absolute clock sources: " + error);
        llvm::json::Array clocks;
        for (auto &source : sources)
          clocks.push_back(llvm::json::Object{{"absoluteSource", routes[source.routeIndex].absoluteSource},
              {"srcClockPort", source.sourceTarget}});
        llvm::SmallString<256> clockPath(outputDir);
        llvm::sys::path::append(clockPath, "print-clock-sources.json");
        llvm::raw_fd_ostream clockMetadata(clockPath, ec);
        if (ec) return fail("cannot write print clock sources: " + ec.message());
        clockMetadata << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(clocks)));
        llvm::outs() << "Resolved " << sources.size() << " absolute printf clock sources; loopbacks pending\n";
      }
    }
    return 0;
  };
  if (synthesizePrintStubs) {
    std::string error;
    if (failed(goldengate::lowerTypesWithRetainedTargets(*module, circuit, error)))
      return fail("PrintSynthesis LowerTypes: " + error);
    mlir::PassManager lowForm(module->getContext());
    lowForm.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
    if (failed(lowForm.run(*module))) return fail("PrintSynthesis ExpandWhens failed");
    return emitPrintStubBoundary();
  }

  if (gateAutoCounter || gateSelectedAutoCounter || synthesizeAutoCounterValues || synthesizeAutoCounterPrints) {
    // The explicit selected boundary applies Scala's generated-cover filter.
    // The original boundary continues to accept already selected records.
    // Enabled AutoCounter remains an incremental debug pipeline boundary.
    std::string error;
    if (analyzeAutoCounterPrintClocks) {
      // Match MidasTransforms: extract the clock bridge before lowering and
      // before AutoCounter/PrintSynthesis. Its clock output becomes a top input.
      if (failed(goldengate::promoteBridgePorts(circuit, error, true)))
        return fail("PrintSynthesis BridgeExtraction: " + error);
      if (failed(mlir::verify(*module)))
        return fail("PrintSynthesis BridgeExtraction produced invalid FIRRTL IR");
      llvm::SmallString<256> firPath(outputDir), annoPath(outputDir);
      llvm::sys::path::append(firPath, "post-bridge-extraction.fir");
      llvm::sys::path::append(annoPath, "post-bridge-extraction-all.json");
      std::error_code ec;
      llvm::raw_fd_ostream firOut(firPath, ec);
      if (ec) return fail("cannot write printf bridge extraction: " + ec.message());
      if (failed(exportFIRFile(*module, firOut, std::nullopt, exportFIRVersion)))
        return fail("cannot export printf bridge extraction FIRRTL");
      if (failed(goldengate::emitAllAnnotations(circuit, annoPath, error)))
        return fail("cannot export printf bridge extraction annotations: " + error);
    }
    if (failed(goldengate::lowerTypesWithRetainedTargets(*module, circuit, error)))
      return fail("AutoCounter LowerTypes: " + error);
    // SFC MiddleFirrtlToLowFirrtl expands whens before AutoCounter: named
    // events declared inside a when must dominate the appended reset gates.
    mlir::PassManager lowForm(module->getContext());
    lowForm.nest<CircuitOp>().addNestedPass<FModuleOp>(createExpandWhensPass());
    if (failed(lowForm.run(*module)))
      return fail("AutoCounter ExpandWhens failed");
    llvm::SmallVector<goldengate::AutoCounterEvent> events;
    llvm::SmallVector<llvm::StringRef> coverModules;
    std::unique_ptr<llvm::MemoryBuffer> coverBuffer;
    if (gateSelectedAutoCounter || synthesizeAutoCounterValues || synthesizeAutoCounterPrints) {
      llvm::SmallString<256> coverPath(outputDir);
      llvm::sys::path::append(coverPath, "autocounter-covermodules.txt");
      if (llvm::sys::fs::exists(coverPath)) {
        auto file = llvm::MemoryBuffer::getFile(coverPath);
        if (!file)
          return fail("cannot read AutoCounter cover modules: " +
                      file.getError().message());
        coverBuffer = std::move(*file);
        // Source.getLines accepts LF, CRLF, and CR. Preserve empty lines and
        // whitespace verbatim: they select nothing unless an exact name exists.
        llvm::StringRef remaining = coverBuffer->getBuffer();
        while (!remaining.empty()) {
          auto end = remaining.find_first_of("\r\n");
          coverModules.push_back(remaining.take_front(end));
          if (end == llvm::StringRef::npos)
            break;
          bool cr = remaining[end] == '\r';
          remaining = remaining.drop_front(end + 1);
          if (cr)
            remaining.consume_front("\n");
        }
      }
    }
    auto analyzed = (gateSelectedAutoCounter || synthesizeAutoCounterValues || synthesizeAutoCounterPrints)
        ? goldengate::analyzeSelectedAutoCounterEvents(
              circuit, coverModules, events, error)
        : goldengate::analyzeAutoCounterEvents(circuit, events, error);
    if (failed(analyzed) || failed(goldengate::gateAutoCounterEventsWithReset(
                                circuit, events, error)))
      return fail("AutoCounter reset gating: " + error);
    if (synthesizeAutoCounterValues || synthesizeAutoCounterPrints) {
      // Reset gating renamed the selected targets; resolve their new SSA values
      // before synthesizing state and comparisons against the retained records.
      events.clear();
      llvm::SmallVector<goldengate::AutoCounterPrintfValue> values;
      llvm::SmallVector<goldengate::AutoCounterPrintf> prints;
      if (failed(goldengate::analyzeSelectedAutoCounterEvents(
              circuit, coverModules, events, error)))
        return fail("AutoCounter printf selection: " + error);
      if (synthesizeAutoCounterPrints) {
        if (failed(goldengate::synthesizeAutoCounterPrintf(circuit, events, prints, error)))
          return fail("AutoCounter printf: " + error);
        for (auto &print : prints) values.push_back(print.value);
      } else if (failed(goldengate::synthesizeAutoCounterPrintfValues(
                     circuit, events, values, error)))
        return fail("AutoCounter printf values: " + error);
      llvm::json::Array summary;
      for (auto &value : values) {
        auto reg = cast<RegResetOp>(value.state.getDefiningOp());
        summary.push_back(llvm::json::Object{
            {"target", value.source.target},
            {"clock", value.source.clockTarget},
            {"reset", value.source.resetTarget},
            {"label", value.source.label},
            {"mode", value.mode == goldengate::AutoCounterMode::Accumulate
                         ? "Accumulate" : "Identity"},
            {"state", reg.getName().str()},
            {"state_width", *cast<UIntType>(value.state.getType()).getWidth()},
            {"event_width", *cast<UIntType>(value.source.event.getType()).getWidth()}});
      }
      llvm::SmallString<256> summaryPath(outputDir);
      llvm::sys::path::append(summaryPath, "autocounter-printf-values.json");
      std::error_code ec;
      llvm::raw_fd_ostream out(summaryPath, ec);
      if (ec) return fail("cannot write AutoCounter values: " + ec.message());
      out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(summary)));
    }
    if (synthesizeAutoCounterStubs) return emitPrintStubBoundary();
    if (failed(mlir::verify(*module)))
      return fail("AutoCounter reset gating produced invalid FIRRTL IR");
    llvm::SmallString<256> irPath(outputDir), annoPath(outputDir);
    llvm::sys::path::append(irPath, synthesizeAutoCounterPrints
        ? "post-autocounter-printf.mlir" : synthesizeAutoCounterValues
        ? "post-autocounter-printf-values.mlir" : "post-autocounter-reset-gate.mlir");
    llvm::sys::path::append(annoPath, synthesizeAutoCounterPrints
        ? "post-autocounter-printf-all.json" : synthesizeAutoCounterValues
        ? "post-autocounter-printf-values-all.json" : "post-autocounter-reset-gate-all.json");
    std::error_code writeError;
    llvm::raw_fd_ostream irOut(irPath, writeError);
    if (writeError)
      return fail("cannot write AutoCounter IR: " + writeError.message());
    module->print(irOut);
    irOut << '\n';
    // Keep the lowered boundary in the FIRRTL dialect: LowerTypes may emit
    // multibit_mux operations that CIRCT's FIR text exporter cannot represent.
    if (failed(goldengate::emitAllAnnotations(circuit, annoPath, error)))
      return fail("cannot export AutoCounter reset-gate boundary: " + error);
    llvm::outs() << (synthesizeAutoCounterPrints ? "Synthesized printf operations for " : synthesizeAutoCounterValues ? "Synthesized printf values for " : "Gated ")
                 << events.size() << " selected AutoCounter events in " << irPath << '\n';
    return 0;
  }

  if (analyzeAutoCounter) {
    std::string error;
    if (mlir::failed(goldengate::lowerTypesWithRetainedTargets(
            *module, circuit, error)))
      return fail("AutoCounter LowerTypes: " + error);
    llvm::SmallVector<goldengate::AutoCounterEvent> events;
    if (mlir::failed(goldengate::analyzeAutoCounterEvents(circuit, events,
                                                          error)))
      return fail("AutoCounter analysis: " + error);
    llvm::json::Array summary;
    for (auto &event : events)
      summary.push_back(llvm::json::Object{
          {"module", event.module.getName().str()},
          {"target", event.target},
          {"clock", event.clockTarget},
          {"reset", event.resetTarget},
          {"label", event.label},
          {"event_op", event.event.getDefiningOp()
                           ? event.event.getDefiningOp()->getName().getStringRef().str()
                           : "firrtl.port"}});
    llvm::SmallString<256> path(outputDir);
    llvm::sys::path::append(path, "autocounter-events.json");
    std::error_code writeError;
    llvm::raw_fd_ostream out(path, writeError);
    if (writeError)
      return fail("cannot write AutoCounter analysis: " +
                  writeError.message());
    out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(summary)));
    llvm::outs() << "Resolved " << events.size()
                 << " AutoCounter events in " << path << '\n';
    return 0;
  }

  if (lowerTypes) {
    std::string error;
    if (mlir::failed(goldengate::lowerTypesWithRetainedTargets(
            *module, circuit, error)))
      return fail("LowerTypes: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("LowerTypes produced invalid FIRRTL IR");
    llvm::SmallString<256> irPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(irPath, "post-lower-types.mlir");
    llvm::sys::path::append(annotationPath, "post-lower-types-all.json");
    std::error_code writeError;
    llvm::raw_fd_ostream irOut(irPath, writeError);
    if (writeError)
      return fail("cannot write lowered MLIR: " + writeError.message());
    module->print(irOut);
    irOut << '\n';
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("cannot export lowered annotations: " + error);
    llvm::outs() << "Lowered FIRRTL types in " << irPath << '\n';
    return 0;
  }

  // Resolve aggregate annotation paths against FIRRTL port types before any
  // lowering changes the port list.  Field IDs, rather than spellings, are the
  // stable identity needed to transfer these annotations through LowerTypes.
  if (resolveDontTouch) {
    auto raw = circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
    if (!raw)
      return fail("input has no retained annotations");
    llvm::json::Array targets;
    for (auto attr : raw) {
      Annotation annotation(attr);
      if (!annotation.isClass(goldengate::AnnotationClasses::DontTouch))
        continue;
      auto spelling = annotation.getMember<mlir::StringAttr>("target");
      if (!spelling)
        return fail("DontTouchAnnotation has no target");
      std::string error;
      auto target = goldengate::resolveAnnotationTarget(
          circuit, spelling.getValue(), error);
      if (!target || !target->port) {
        std::string internalError;
        auto *declaration = goldengate::resolveInternalAnnotationTarget(
            circuit, spelling.getValue(), internalError);
        if (!declaration)
          return fail("unresolved DontTouch target " +
                      spelling.getValue().str() + ": " + internalError);
        targets.push_back(llvm::json::Object{
            {"source", spelling.getValue().str()},
            {"declaration", declaration->getAttrOfType<mlir::StringAttr>(
                                 "name").getValue().str()},
            {"operation", declaration->getName().getStringRef().str()}});
        continue;
      }
      targets.push_back(llvm::json::Object{
          {"source", spelling.getValue().str()},
          {"module", target->module.getModuleName().str()},
          {"port", target->module.getPortName(*target->port).str()},
          {"field_id", *target->fieldID},
          {"ground_port", target->groundPortName}});
    }
    llvm::SmallString<256> path(outputDir);
    llvm::sys::path::append(path, "resolved-dont-touch.json");
    std::error_code writeError;
    llvm::raw_fd_ostream out(path, writeError);
    if (writeError)
      return fail("cannot write resolved targets: " + writeError.message());
    out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(targets)));
    llvm::outs() << "Resolved DontTouch targets in " << path << '\n';
    return 0;
  }

  if (addHostControl) {
    FModuleOp model;
    for (auto &op : circuit.getBodyBlock()->getOperations())
      if (auto candidate = mlir::dyn_cast<FModuleOp>(&op);
          candidate && candidate.getName() == argv[7])
        model = candidate;
    if (!model)
      return fail("FAME model module missing: " + std::string(argv[7]));
    std::string rewriteError;
    if (mlir::failed(goldengate::addFAMEHostControl(circuit, model,
                                                   rewriteError)))
      return fail("FAME host-control construction: " + rewriteError);
    if (mlir::failed(mlir::verify(*module)))
      return fail("FAME host-control construction produced invalid FIRRTL IR");
    llvm::SmallString<256> rewrittenPath(outputDir);
    llvm::sys::path::append(rewrittenPath, "host-control.mlir");
    std::error_code writeError;
    llvm::raw_fd_ostream rewrittenOut(rewrittenPath, writeError);
    if (writeError)
      return fail("cannot write host-control MLIR: " + writeError.message());
    module->print(rewrittenOut);
    rewrittenOut << '\n';
    llvm::outs() << "Added FAME host controls in " << rewrittenPath << '\n';
    return 0;
  }

  // Exercise the FAME channel-control rewrites on an already decoupled module.
  // This is an intermediate CIRCT IR boundary while model port conversion is
  // being ported; normal handoff ingestion retains the original path below.
  if (rewriteOutputValids) {
    auto dependencyBuffer = llvm::MemoryBuffer::getFile(argv[7]);
    if (!dependencyBuffer)
      return fail("cannot read FAME dependency analysis");
    auto parsed = llvm::json::parse((*dependencyBuffer)->getBuffer());
    auto *object = parsed ? parsed->getAsObject() : nullptr;
    auto *rows = object ? object->getArray("local_channel_dependencies")
                        : nullptr;
    auto *channelPorts = object ? object->getArray("channel_ports") : nullptr;
    auto *channelConnections =
        object ? object->getArray("channel_connections") : nullptr;
    if (!rows || !channelPorts || !channelConnections)
      return fail("analysis has no local dependencies or channels");
    std::map<std::string,
             llvm::SmallVector<goldengate::LocalChannelDependency>> byModule;
    // FAMETransformAnnotation selects models independently of their channels.
    // Older boundary reports omit this field and derive selection from rows.
    std::optional<std::set<std::string>> selectedModules;
    if (auto *selection = object->get("transformed_modules")) {
      auto *names = selection->getAsArray();
      if (!names)
        return fail("malformed FAME transformed module analysis");
      selectedModules.emplace();
      for (const auto &name : *names) {
        auto spelling = name.getAsString();
        if (!spelling || spelling->empty())
          return fail("malformed FAME transformed module name");
        selectedModules->insert(spelling->str());
        byModule.try_emplace(spelling->str());
      }
    }
    auto isSelected = [&](llvm::StringRef name) {
      return !selectedModules || selectedModules->count(name.str());
    };
    std::map<std::string, llvm::SmallVector<std::string>> inputsByModule;
    std::map<std::string, llvm::SmallVector<std::string>> outputsByModule;
    std::map<std::string, std::map<std::string, std::optional<std::string>>>
        channelClocksByModule;
    for (auto &row : *channelPorts) {
      auto *entry = row.getAsObject();
      auto moduleName = entry ? entry->getString("module") : std::nullopt;
      auto channelName = entry ? entry->getString("name") : std::nullopt;
      auto direction = entry ? entry->getString("direction") : std::nullopt;
      if (!moduleName || !channelName || !direction)
        return fail("malformed channel port analysis");
      auto *clock = entry->get("clock_port");
      if (!clock || (!clock->getAsNull() && !clock->getAsString()))
        return fail("malformed channel clock analysis");
      channelClocksByModule[moduleName->str()][channelName->str()] =
          clock->getAsString()
              ? std::optional<std::string>(clock->getAsString()->str())
              : std::nullopt;
      if (*direction == "input")
        inputsByModule[moduleName->str()].push_back(channelName->str());
      if (*direction == "output")
        outputsByModule[moduleName->str()].push_back(channelName->str());
      // Scala transforms selected models even when they have no outputs.
      // Such models have no local output dependency rows, but still need
      // input fired state, completion/readies, and buffered target clocks.
      if (isSelected(*moduleName))
        byModule.try_emplace(moduleName->str());
    }
    for (auto &row : *rows) {
      auto *entry = row.getAsObject();
      auto moduleName = entry ? entry->getString("module") : std::nullopt;
      auto outputName = entry ? entry->getString("output_channel")
                              : std::nullopt;
      auto *inputs = entry ? entry->getArray("input_channels") : nullptr;
      auto *unresolved = entry ? entry->getArray("unresolved_ports") : nullptr;
      auto *causes = entry ? entry->getArray("unresolved_causes") : nullptr;
      if (!moduleName || !outputName || !inputs || !unresolved || !causes)
        return fail("malformed local channel dependency");
      goldengate::LocalChannelDependency dependency;
      dependency.outputChannel = outputName->str();
      auto copyStrings = [&](const llvm::json::Array &from,
                             std::vector<std::string> &to) {
        for (const auto &value : from) {
          auto spelling = value.getAsString();
          if (!spelling)
            return false;
          to.push_back(spelling->str());
        }
        return true;
      };
      if (!copyStrings(*inputs, dependency.inputChannels) ||
          !copyStrings(*unresolved, dependency.unresolvedPorts) ||
          !copyStrings(*causes, dependency.unresolvedCauses))
        return fail("malformed local channel dependency names");
      if (isSelected(*moduleName))
        byModule[moduleName->str()].push_back(std::move(dependency));
    }
    for (auto &[moduleName, dependencies] : byModule) {
      FModuleOp model;
      for (auto &op : circuit.getBodyBlock()->getOperations())
        if (auto candidate = mlir::dyn_cast<FModuleOp>(&op);
            candidate && candidate.getName() == moduleName)
          model = candidate;
      if (!model)
        return fail("FAME model module missing: " + moduleName);
      std::string rewriteError;
      // FAMETransformer.isClockChannel examines this model's input payloads,
      // not names from circuit-wide channel connections. A data channel in a
      // non-hub may share a name with another model's explicit clock channel.
      std::set<std::string> clockChannelPorts;
      llvm::SmallVector<std::string> dataInputs;
      for (const auto &name : inputsByModule[moduleName]) {
        BundleType payload;
        for (unsigned i = 0; i < model.getNumPorts(); ++i)
          if (model.getPortName(i) == name + "_sink" &&
              model.getPortDirection(i) == Direction::In)
            payload = mlir::dyn_cast<BundleType>(model.getPorts()[i].type);
        if (!payload)
          return fail("missing decoupled input channel in " + moduleName +
                      ": " + name);
        auto bits = payload.getElementIndex("bits");
        bool clockPayload = bits &&
            mlir::isa<ClockType>(payload.getElements()[*bits].type);
        if (clockPayload) {
          if (channelClocksByModule[moduleName].at(name))
            return fail("clock channel has an associated clock in " +
                        moduleName + ": " + name);
          clockChannelPorts.insert(name);
        } else {
          dataInputs.push_back(name);
        }
      }
      inputsByModule[moduleName] = std::move(dataInputs);
      // FAMETransformer uses And.reduce on data channel conditions, whose
      // Scala implementation requires a nonempty sequence. A clock channel
      // alone does not satisfy that contract. Diagnose before clock rewrites.
      if (inputsByModule[moduleName].empty() &&
          outputsByModule[moduleName].empty())
        return fail("FAME model has no data channels: " + moduleName);
      // No explicit target-clock channel denotes a non-hub single-clock
      // model. SFC gives its VirtualClockChannel constant valid/enable bits.
      // Preserve each data channel's optional clock association: Some(clock)
      // inputs reset fired, whereas None channels reset unfired.
      if (clockChannelPorts.size() > 1)
        return fail("FAME control boundary supports one target clock channel "
                    "per model: " + moduleName);
      bool virtualClock = clockChannelPorts.empty();
      llvm::StringRef clockName = virtualClock ? llvm::StringRef()
                                               : *clockChannelPorts.begin();
      mlir::Value inputEnable, outputEnable;
      mlir::OpBuilder builder(&model.getBodyBlock()->front());
      if (virtualClock) {
        std::set<std::string> clocks;
        for (const auto &[name, clock] : channelClocksByModule[moduleName])
          if (clock)
            clocks.insert(*clock);
        if (clocks.size() > 1)
          return fail("virtual-clock model has multiple channel clocks: " +
                      moduleName);
        // The virtual token has no decoupled port. Buffer constant one at
        // completion and gate all reads of the original single target clock,
        // exactly as FAMETransformer.targetClockMetadata does for non-hubs.
        std::optional<std::string> targetClock;
        for (unsigned i = 0; i < model.getNumPorts(); ++i)
          if (model.getPortDirection(i) == Direction::In &&
              model.getPortName(i) != "hostClock" &&
              mlir::isa<ClockType>(model.getPorts()[i].type)) {
            if (targetClock)
              return fail("virtual-clock model has multiple target clocks: " +
                          moduleName);
            targetClock = model.getPortName(i).str();
          }
        if (!targetClock || (!clocks.empty() && *clocks.begin() != *targetClock))
          return fail("virtual-clock model lacks its associated target clock: " +
                      moduleName);
        if (failed(goldengate::addFAMEClockEnable(
                model, *targetClock, {}, rewriteError)) ||
            failed(goldengate::addFAMEClockGate(
                circuit, model, *targetClock, rewriteError)) ||
            failed(goldengate::removeFAMEVirtualClockPort(
                circuit, model, *targetClock, rewriteError)))
          return fail("FAME virtual-clock construction: " + rewriteError);
        // Keep control operands before the newly prepended declarations.
        builder.setInsertionPoint(&model.getBodyBlock()->front());
        inputEnable = builder.create<ConstantOp>(
            model.getLoc(), UIntType::get(&context, 1), llvm::APInt(1, 1));
      } else {
        std::string clockSinkName = (clockName + "_sink").str();
        std::string clockEnableName = (clockName + "_enabled").str();
        mlir::Value clockPort;
        for (unsigned i = 0, n = model.getPorts().size(); i < n; ++i)
          if (model.getPortName(i) == clockSinkName &&
              model.getPortDirection(i) == Direction::In)
            clockPort = model.getBodyBlock()->getArgument(i);
        model.walk([&](RegResetOp op) {
          if (op.getName() == clockEnableName)
            outputEnable = op.getResult();
        });
        if (!clockPort || !outputEnable)
          return fail("FAME clock port or enable register missing in " +
                      moduleName);
        mlir::Value clockBits =
            builder.create<SubfieldOp>(model.getLoc(), clockPort, "bits");
        inputEnable =
            builder.create<AsUIntPrimOp>(model.getLoc(), clockBits).getResult();
      }
      llvm::SmallVector<goldengate::FAMEFiredChannel> firedChannels;
      auto addFiredChannel = [&](const std::string &name, bool isInput) {
        const auto &clock = channelClocksByModule[moduleName].at(name);
        if (!clock && !virtualClock) {
          rewriteError = "channel has no associated clock: " + name;
          return false;
        }
        mlir::Value enable = inputEnable;
        if (clock && !isInput) {
          enable = outputEnable;
          if (virtualClock)
            model.walk([&](RegResetOp op) {
              if (op.getName() == *clock + "_enabled")
                enable = op.getResult();
            });
          if (!enable) {
            rewriteError = "missing buffered channel clock enable for " + name;
            return false;
          }
        }
        firedChannels.push_back({name, isInput, enable, bool(clock)});
        return true;
      };
      for (const auto &name : inputsByModule[moduleName])
        if (!addFiredChannel(name, true))
          return fail("FAME fired-state metadata: " + rewriteError);
      for (const auto &name : outputsByModule[moduleName])
        if (!addFiredChannel(name, false))
          return fail("FAME fired-state metadata: " + rewriteError);
      if (mlir::failed(goldengate::ensureFAMEFiredRegisters(
              model, firedChannels, rewriteError)))
        return fail("FAME fired-register creation: " + rewriteError);
      if (mlir::failed(goldengate::rewriteFAMEOutputValids(
              model, dependencies, rewriteError)))
        return fail("FAME output-valid rewrite: " + rewriteError);
      if (mlir::failed(goldengate::rewriteFAMEInputReadies(
              model, inputsByModule[moduleName], rewriteError)))
        return fail("FAME input-ready rewrite: " + rewriteError);
      if (mlir::failed(goldengate::rewriteFAMEFiredStates(
              model, firedChannels, rewriteError)))
        return fail("FAME fired-state rewrite: " + rewriteError);
      if (mlir::failed(goldengate::rewriteFAMEFinishing(
              model, inputsByModule[moduleName], outputsByModule[moduleName],
              clockName, rewriteError)))
        return fail("FAME finishing rewrite: " + rewriteError);
    }
    if (mlir::failed(mlir::verify(*module)))
      return fail("FAME channel-control rewrites produced invalid FIRRTL IR");
    llvm::SmallString<256> rewrittenPath(outputDir);
    llvm::sys::path::append(rewrittenPath, "output-valid.mlir");
    std::error_code writeError;
    llvm::raw_fd_ostream rewrittenOut(rewrittenPath, writeError);
    if (writeError)
      return fail("cannot write rewritten MLIR: " + writeError.message());
    module->print(rewrittenOut);
    rewrittenOut << '\n';
    llvm::outs() << "Rewrote FAME channel controls and fired state in "
                 << rewrittenPath << '\n';
    return 0;
  }

  // CIRCT's FIR parser leaves unknown Golden Gate classes as rawAnnotations.
  // Keep them in the IR until a dedicated GG pass has explicitly consumed or
  // transferred each class. Count the imported attributes, not just the JSON.
  auto rawAnnotations = circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
  if (!rawAnnotations && parsedAnnotations->getAsArray()->empty()) {
    rawAnnotations = mlir::ArrayAttr::get(&context, {});
    circuit->setAttr("rawAnnotations", rawAnnotations);
  }
  if (!rawAnnotations)
    return fail("CIRCT did not retain the input annotations");
  // The current FireSim generator writes the same annotations into the FIRRTL
  // 3.3 header and the separate .anno.json. SFC's handoff provides both. Only
  // remove the second copy when every imported attribute matches in order.
  bool deduplicatedEmbeddedAnnotations = false;
  auto externalCount = parsedAnnotations->getAsArray()->size();
  if (externalCount && rawAnnotations.size() == 2 * externalCount &&
      llvm::equal(rawAnnotations.getValue().take_front(externalCount),
                  rawAnnotations.getValue().drop_front(externalCount))) {
    rawAnnotations = mlir::ArrayAttr::get(
        &context, rawAnnotations.getValue().take_front(externalCount));
    circuit->setAttr("rawAnnotations", rawAnnotations);
    deduplicatedEmbeddedAnnotations = true;
  }
  std::map<std::string, unsigned> classes;
  for (auto attr : rawAnnotations) {
    Annotation annotation(attr);
    ++classes[annotation.getClass().empty() ? "<classless>"
                                           : annotation.getClass().str()];
  }
  // FIRRTL 3.3 files may also embed an annotation array. The imported IR must
  // include at least every class supplied in the separate handoff JSON.
  std::map<std::string, unsigned> externalClasses;
  for (const auto &value : *parsedAnnotations->getAsArray()) {
    auto *object = value.getAsObject();
    if (!object)
      return fail("annotation array contains a non-object entry");
    auto name = object->getString("class");
    ++externalClasses[name ? name->str() : "<classless>"];
  }
  for (const auto &[name, count] : externalClasses)
    if (classes[name] < count)
      return fail("CIRCT lost input annotations of class " + name);

  if (updateBridgeClocks) {
    std::string error;
    if (mlir::failed(goldengate::analyzeChannelClocksAndUpdateBridges(
            circuit, error)))
      return fail("ChannelClockInfo/UpdateBridgeClockInfo: " + error);
    llvm::SmallString<256> annotationPath(outputDir);
    llvm::sys::path::append(annotationPath, "post-bridge-clocks-all.json");
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("bridge clock annotation emission: " + error);
    llvm::outs() << "Resolved bridge clocks in " << annotationPath << '\n';
    return 0;
  }

  if (labelMultiThreaded) {
    std::string error;
    bool enabled = llvm::StringRef(argv[6]).ends_with("=on");
    if (mlir::failed(goldengate::labelMultiThreadedInstances(circuit,
                                                              enabled, error)))
      return fail("LabelMultiThreadedInstances: " + error);
    llvm::SmallString<256> annotationPath(outputDir);
    llvm::sys::path::append(annotationPath,
                            "post-label-multithreaded-all.json");
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("LabelMultiThreadedInstances annotation emission: " + error);
    llvm::outs() << "Labeled multithreaded model annotations in "
                 << annotationPath << '\n';
    return 0;
  }

  if (wrapTop) {
    std::string error;
    if (mlir::failed(goldengate::wrapTop(circuit, error)))
      return fail("WrapTop: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("WrapTop produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-wrap-top.fir");
    llvm::sys::path::append(annotationPath, "post-wrap-top-all.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write wrapped FIRRTL: " + writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export wrapped FIRRTL");
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("WrapTop annotation emission: " + error);
    llvm::outs() << "Wrapped top in " << firPath << '\n';
    return 0;
  }

  if (extractModels) {
    std::string error;
    unsigned promoted = 0;
    if (mlir::failed(goldengate::extractModels(circuit, promoted, error)))
      return fail("ExtractModel: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("ExtractModel produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-extract-model.fir");
    llvm::sys::path::append(annotationPath, "post-extract-model-all.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write extracted model FIRRTL: " +
                  writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export extracted model FIRRTL");
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    error)))
      return fail("ExtractModel annotation emission: " + error);
    llvm::outs() << "Promoted " << promoted << " model instances in "
                 << firPath << '\n';
    return 0;
  }

  if (promotePassthrough) {
    std::string error;
    unsigned promoted = 0;
    if (mlir::failed(goldengate::promotePassthroughConnections(
            circuit, promoted, error)))
      return fail("PromotePassthroughConnections: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("PromotePassthroughConnections produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-promote-passthrough.fir");
    llvm::sys::path::append(annotationPath, "post-promote-passthrough.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write passthrough FIRRTL: " + writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export passthrough FIRRTL");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, annotationPath,
                                                     error)))
      return fail("passthrough annotation emission: " + error);
    llvm::outs() << "Promoted " << promoted << " passthrough connections in "
                 << firPath << '\n';
    return 0;
  }

  if (applyFAMEDefaults) {
    std::string error;
    if (mlir::failed(goldengate::addFAMEDefaults(circuit, error)))
      return fail("FAMEDefaults: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("FAMEDefaults produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-fame-defaults.fir");
    llvm::sys::path::append(annotationPath, "post-fame-defaults.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write FAMEDefaults FIRRTL: " + writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export FAMEDefaults FIRRTL");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, annotationPath,
                                                     error)))
      return fail("FAMEDefaults annotation emission: " + error);
    llvm::outs() << "Applied CIRCT FAMEDefaults to " << firPath << '\n';
    return 0;
  }

  if (inferDefaultClocks) {
    std::string error;
    if (mlir::failed(goldengate::findDefaultClocks(circuit, error)))
      return fail("FindDefaultClocks: " + error);
    llvm::SmallString<256> path(outputDir);
    llvm::sys::path::append(path, "post-find-default-clocks.json");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, path, error)))
      return fail("FindDefaultClocks annotation emission: " + error);
    llvm::outs() << "Inferred CIRCT model clocks in " << path << '\n';
    return 0;
  }

  if (exciseChannels) {
    std::string error;
    if (mlir::failed(goldengate::exciseChannels(circuit, error)))
      return fail("ChannelExcision: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("ChannelExcision produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-channel-excision.fir");
    llvm::sys::path::append(annotationPath, "post-channel-excision.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write ChannelExcision FIRRTL: " + writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export ChannelExcision FIRRTL");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, annotationPath,
                                                     error)))
      return fail("ChannelExcision annotation emission: " + error);
    llvm::outs() << "Excised CIRCT channels into " << firPath << '\n';
    return 0;
  }

  if (inferModelPorts) {
    std::string error;
    if (mlir::failed(goldengate::inferModelPorts(circuit, error)))
      return fail("InferModelPorts: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("InferModelPorts produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir), annotationPath(outputDir);
    llvm::sys::path::append(firPath, "post-infer-model-ports.fir");
    llvm::sys::path::append(annotationPath, "post-infer-model-ports.json");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write InferModelPorts FIRRTL: " + writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export InferModelPorts FIRRTL");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, annotationPath,
                                                     error)))
      return fail("InferModelPorts annotation emission: " + error);
    llvm::outs() << "Inferred CIRCT model ports into " << firPath << '\n';
    return 0;
  }

  if (promoteGroundBridges || promoteAggregateBridges) {
    std::string error;
    if (mlir::failed(goldengate::promoteBridgePorts(
            circuit, error, promoteAggregateBridges)))
      return fail("BridgePromotion: " + error);
    if (mlir::failed(mlir::verify(*module)))
      return fail("BridgePromotion produced invalid FIRRTL IR");
    llvm::SmallString<256> firPath(outputDir);
    llvm::sys::path::append(firPath, promoteAggregateBridges
                                        ? "post-aggregate-bridge-promotion.fir"
                                        : "post-ground-bridge-promotion.fir");
    std::error_code writeError;
    llvm::raw_fd_ostream firOut(firPath, writeError);
    if (writeError)
      return fail("cannot write BridgePromotion FIRRTL: " +
                  writeError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export BridgePromotion FIRRTL");
    if (promoteAggregateBridges) {
      llvm::SmallString<256> annotationPath(outputDir);
      llvm::sys::path::append(annotationPath,
                              "post-aggregate-bridge-promotion.json");
      // Subsequent Golden Gate passes consume non-FAME annotations too (for
      // example FirrtlMemModelAnnotation). Preserve the complete annotation
      // state at this FIRRTL handoff boundary.
      if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                     error)))
        return fail("cannot export BridgePromotion annotations: " + error);
    }
    llvm::outs() << "Promoted CIRCT bridge ports into " << firPath << '\n';
    return 0;
  }

  std::string bridgeError;
  auto bridges = goldengate::analyzeBridgeInstances(circuit, bridgeError);
  if (!bridges)
    return fail("invalid bridge hierarchy: " + bridgeError);
  llvm::json::Array bridgeInstances;
  for (const auto &bridge : *bridges) {
    auto bridgeModule = bridge.module;
    llvm::json::Array path, channelNames, topChannelNames;
    for (const auto &part : bridge.path)
      path.push_back(part);
    for (const auto &name : bridge.channelNames) {
      channelNames.push_back(name);
      if (bridge.path.size() == 1)
        topChannelNames.push_back(bridge.path.front() + "_" + name);
    }
    bridgeInstances.push_back(llvm::json::Object{
        {"path", std::move(path)},
        {"module", bridgeModule.getModuleName().str()},
        {"widget_class", bridge.widgetClass},
        {"channel_names", std::move(channelNames)},
        {"top_channel_names", std::move(topChannelNames)}});
  }

  std::string hierarchyError;
  auto hierarchy = goldengate::analyzeTopHierarchy(circuit, hierarchyError);
  if (!hierarchy)
    return fail("invalid top hierarchy: " + hierarchyError);
  llvm::json::Array topConnections;
  for (auto connection : hierarchy->connections)
    topConnections.push_back(llvm::json::Object{
        {"top_port", hierarchy->top.getPortName(connection.topPort).str()},
        {"instance", connection.instance.getName().str()},
        {"module", connection.instance.getModuleName().str()},
        {"instance_port", connection.instance.getPortNameStr(connection.instancePort).str()}});

  // InferModelPorts describes channels on module ports. Resolve those SFC
  // targets once, then keep CIRCT module operations and port indices as the
  // identities used by the later channel and FAME analyses.
  llvm::json::Array channelPorts, channelConnections, transformedModules;
  llvm::SmallVector<goldengate::GGChannelConnection, 0> analyzedChannels;
  llvm::SmallVector<goldengate::ModelPortGroup> modelPortGroups;
  llvm::SmallVector<FModuleLike> transformedModelOps;
  auto portName = [](const goldengate::GGTarget &target) {
    auto module = target.module;
    return module.getPortName(*target.port).str();
  };
  for (auto attr : rawAnnotations) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
      std::string error;
      auto connection = goldengate::analyzeChannelConnection(circuit, annotation,
                                                             error);
      if (!connection)
        return fail("invalid channel connection: " + error);
      llvm::json::Array sources, sinks, targetClocks;
      llvm::json::Object handshake;
      for (const auto &target : connection->sources)
        sources.push_back(portName(target));
      for (const auto &target : connection->sinks)
        sinks.push_back(portName(target));
      for (const auto &[field, target] : connection->handshake)
        handshake[field] = portName(target);
      for (const auto &clock : connection->targetClocks)
        targetClocks.push_back(llvm::json::Object{
            {"name", clock.name}, {"multiplier", clock.multiplier},
            {"divisor", clock.divisor}, {"mfmr", clock.mfmr}});
      const char *kind = nullptr;
      switch (connection->kind) {
      case goldengate::ChannelKind::Pipe: kind = "pipe"; break;
      case goldengate::ChannelKind::DecoupledForward: kind = "decoupled_forward"; break;
      case goldengate::ChannelKind::DecoupledReverse: kind = "decoupled_reverse"; break;
      case goldengate::ChannelKind::TargetClock: kind = "target_clock"; break;
      }
      channelConnections.push_back(llvm::json::Object{
          {"name", connection->name}, {"kind", kind},
          {"clock_port", connection->clock ? llvm::json::Value(portName(*connection->clock))
                                           : llvm::json::Value(nullptr)},
          {"sources", std::move(sources)}, {"sinks", std::move(sinks)},
          {"handshake", std::move(handshake)},
          {"latency", connection->latency ? llvm::json::Value(*connection->latency)
                                            : llvm::json::Value(nullptr)},
          {"target_clocks", std::move(targetClocks)}});
      analyzedChannels.push_back(std::move(*connection));
      continue;
    }
    if (annotation.isClass(goldengate::AnnotationClasses::FAMETransform)) {
      auto target = annotation.getMember<mlir::StringAttr>("target");
      if (!target)
        return fail("FAMETransformAnnotation has no target");
      std::string error;
      auto resolved = goldengate::resolveAnnotationTarget(
          circuit, target.getValue(), error);
      if (!resolved || resolved->port)
        return fail("invalid FAME transform module target: " + error);
      transformedModules.push_back(resolved->module.getModuleName().str());
      transformedModelOps.push_back(resolved->module);
      continue;
    }
    if (!annotation.isClass(goldengate::AnnotationClasses::ChannelPorts))
      continue;
    std::string error;
    auto group = goldengate::analyzeModelPortGroup(circuit, annotation, error);
    if (!group)
      return fail("invalid model channel ports: " + error);
    llvm::json::Array portNames;
    for (unsigned port : group->ports)
      portNames.push_back(group->module.getPortName(port).str());

    llvm::json::Value clockPort(nullptr);
    if (group->clockPort)
      clockPort = group->module.getPortName(*group->clockPort).str();
    channelPorts.push_back(llvm::json::Object{
        {"name", group->name},
        {"module", group->module.getModuleName().str()},
        {"direction", group->direction == Direction::In ? "input" : "output"},
        {"clock_port", std::move(clockPort)},
        {"ports", std::move(portNames)}});
    modelPortGroups.push_back(std::move(*group));
  }

  std::string channelPairError;
  if (!goldengate::validateDecoupledChannelPairs(analyzedChannels,
                                                  channelPairError))
    return fail("invalid decoupled channel pair: " + channelPairError);

  llvm::json::Array modelChannelBindings;
  llvm::SmallVector<goldengate::ModelChannelBinding> typedBindings;
  for (const auto &channel : analyzedChannels) {
    std::string error;
    auto bindings = goldengate::bindChannelToModels(
        channel, *hierarchy, modelPortGroups, error);
    if (!bindings)
      return fail("invalid model channel binding: " + error);
    for (auto &binding : *bindings) {
      llvm::json::Array ports;
      auto modelModule = binding.portGroup->module;
      for (unsigned port : binding.instancePorts)
        ports.push_back(modelModule.getPortName(port).str());
      modelChannelBindings.push_back(llvm::json::Object{
          {"global_name", binding.globalName},
          {"local_name", binding.portGroup->name},
          {"model_instance", binding.instance.getName().str()},
          {"model_module", modelModule.getModuleName().str()},
          {"direction", binding.portGroup->direction == Direction::In
                            ? "input" : "output"},
          {"model_clock_port", binding.portGroup->clockPort
                                   ? llvm::json::Value(modelModule.getPortName(
                                           *binding.portGroup->clockPort).str())
                                   : llvm::json::Value(nullptr)},
          {"ports", std::move(ports)}});
      typedBindings.push_back(binding);
    }
  }

  std::string fameError;
  auto famePlan = goldengate::analyzeFAMEPorts(
      *hierarchy, typedBindings, analyzedChannels, transformedModelOps, fameError);
  if (!famePlan)
    return fail("invalid FAME top port plan: " + fameError);
  if (rewriteOutputChannel) {
    llvm::StringRef name(argv[7]);
    auto selected = llvm::find_if(famePlan->sources, [&](const auto &port) {
      return port.binding->globalName == name;
    });
    if (name.empty() || selected == famePlan->sources.end())
      return fail("FAME output channel is missing: " + name.str());
    const auto &binding = *selected->binding;
    if (binding.instancePorts.empty())
      return fail("FAME output channel has no model ports");
    auto model = binding.portGroup->module;
    struct TargetRename {
      std::string oldTop, newTop, oldModel, newModel;
    };
    llvm::SmallVector<TargetRename> renames;
    auto removeCommonPrefix = [](llvm::StringRef portName,
                                 llvm::StringRef channelName) {
      while (!portName.empty() && !channelName.empty() &&
             portName.front() == channelName.front()) {
        portName = portName.drop_front();
        channelName = channelName.drop_front();
      }
      return portName;
    };
    for (unsigned modelPort : binding.instancePorts) {
      std::optional<unsigned> topPort;
      for (const auto &connection : hierarchy->connections)
        if (connection.instance == binding.instance &&
            connection.instancePort == modelPort) {
          if (topPort)
            return fail("FAME output channel has multiple top connections");
          topPort = connection.topPort;
        }
      if (!topPort)
        return fail("FAME output channel has no top port");
      auto oldTopName = hierarchy->top.getPortName(*topPort);
      auto oldModelName = model.getPortName(modelPort);
      std::string topSuffix, modelSuffix;
      if (binding.instancePorts.size() > 1) {
        topSuffix = ("." + removeCommonPrefix(oldTopName,
                                                binding.globalName)).str();
        modelSuffix = ("." + removeCommonPrefix(
                                 oldModelName, binding.portGroup->name)).str();
      }
      std::string topPrefix = "~" + circuit.getName().str() + "|" +
                              hierarchy->top.getName().str() + ">";
      std::string modelPrefix = "~" + circuit.getName().str() + "|" +
                                model.getModuleName().str() + ">";
      renames.push_back({topPrefix + oldTopName.str(),
                         topPrefix + selected->portName + ".bits" + topSuffix,
                         modelPrefix + oldModelName.str(),
                         modelPrefix + binding.portGroup->name +
                             "_source.bits" + modelSuffix});
    }
    std::string rewriteError;
    if (mlir::failed(goldengate::rewriteFAMEOutputChannel(
            *hierarchy, *selected, rewriteError)))
      return fail("FAME output channel rewrite: " + rewriteError);
    llvm::SmallVector<mlir::Attribute> annotations;
    unsigned topTargets = 0, modelTargets = 0;
    for (auto attr : circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attr);
      auto globalName = annotation.getMember<mlir::StringAttr>("globalName");
      auto localName = annotation.getMember<mlir::StringAttr>("localName");
      auto rename = [&](llvm::StringRef member, llvm::StringRef from,
                        llvm::StringRef to) {
        auto entries = annotation.getMember<mlir::ArrayAttr>(member);
        if (!entries)
          return 0u;
        llvm::SmallVector<mlir::Attribute> updated;
        unsigned changed = 0;
        for (auto entry : entries) {
          if (auto value = mlir::dyn_cast<mlir::StringAttr>(entry);
              value && value.getValue() == from) {
            updated.push_back(mlir::StringAttr::get(&context, to));
            ++changed;
          } else {
            updated.push_back(entry);
          }
        }
        if (changed)
          annotation.setMember(member, mlir::ArrayAttr::get(&context, updated));
        return changed;
      };
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection) &&
          globalName && globalName.getValue() == name)
        for (const auto &entry : renames)
          topTargets += rename("sources", entry.oldTop, entry.newTop);
      // SFC's RenameMap also updates handshake cross references.
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
        if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
          mlir::NamedAttrList updated(info);
          for (const auto &entry : renames)
            for (llvm::StringRef field : {"readySource", "validSource"})
              if (auto value = info.getAs<mlir::StringAttr>(field);
                  value && value.getValue() == entry.oldTop)
                updated.set(field,
                            mlir::StringAttr::get(&context, entry.newTop));
          annotation.setMember("channelInfo", updated.getDictionary(&context));
        }
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts) &&
          localName && localName.getValue() == binding.portGroup->name)
        for (const auto &entry : renames)
          modelTargets += rename("ports", entry.oldModel, entry.newModel);
      annotations.push_back(annotation.getAttr());
    }
    if (topTargets != renames.size() || modelTargets != renames.size())
      return fail("FAME output channel annotation targets were not unique");
    circuit->setAttr("rawAnnotations", mlir::ArrayAttr::get(&context, annotations));
    if (mlir::failed(mlir::verify(*module)))
      return fail("FAME output channel rewrite produced invalid FIRRTL IR");
    llvm::SmallString<256> rewrittenPath(outputDir);
    llvm::sys::path::append(rewrittenPath, "output-channel.mlir");
    std::error_code writeError;
    llvm::raw_fd_ostream out(rewrittenPath, writeError);
    if (writeError)
      return fail("cannot write FAME output channel MLIR: " +
                  writeError.message());
    module->print(out);
    out << '\n';
    llvm::outs() << "Rewrote FAME output channel " << name << " in "
                 << rewrittenPath << '\n';
    return 0;
  }
  if (rewriteInputChannel || rewriteInputsWithOutput) {
    // transformTop keeps non-stale ports before newly decoupled channels.
    // Capture their original order before the channel/host-port rewrites and
    // grouping mutate port indices and interleave inputs with outputs.
    llvm::SmallVector<std::string> retainedTopPortNames;
    for (unsigned i = 0; i < hierarchy->top.getNumPorts(); ++i)
      if (!llvm::is_contained(famePlan->staleTopPorts, i))
        retainedTopPortNames.push_back(hierarchy->top.getPortName(i).str());
    llvm::SmallVector<llvm::StringRef> requestedNames;
    if (rewriteInputsWithOutput || llvm::StringRef(argv[7]) == "all") {
      // Preserve FAME's channel order when creating ports and fired state,
      // and leave the target clock token to its own rewrite.
      auto targetClock = llvm::find_if(analyzedChannels, [](const auto &channel) {
        return channel.kind == goldengate::ChannelKind::TargetClock;
      });
      if (targetClock == analyzedChannels.end())
        return fail("FAME input channels have no target clock channel");
      for (const auto &port : famePlan->sinks)
        if (!port.binding->instancePorts.empty() &&
            port.binding->globalName != targetClock->name)
          requestedNames.push_back(port.binding->globalName);
    } else {
      llvm::StringRef(argv[7]).split(requestedNames, ',', -1, true);
    }
    if (requestedNames.empty())
      return fail("no FAME data input channels were selected");
    llvm::SmallVector<const goldengate::FAMETopChannelPort *> selectedPorts;
    std::set<std::string> seenNames;
    for (llvm::StringRef name : requestedNames) {
      if (name.empty() || !seenNames.insert(name.str()).second)
        return fail("FAME input channel names must be nonempty and unique");
      auto selected = llvm::find_if(famePlan->sinks, [&](const auto &port) {
        return port.binding->globalName == name;
      });
      if (selected == famePlan->sinks.end())
        return fail("FAME input channel is missing: " + name.str());
      if (selected->binding->instancePorts.empty())
        return fail("FAME input channel has no model ports: " + name.str());
      selectedPorts.push_back(&*selected);
    }
    auto oldModel = selectedPorts.front()->binding->portGroup->module;
    // Use the complete annotation graph, including output channels that are
    // outside this partial rewrite, to identify SFC's unusedOutputsAsWires.
    std::set<unsigned> channelOutputPorts;
    for (const auto &group : modelPortGroups)
      if (group.module == oldModel && group.direction == Direction::Out)
        channelOutputPorts.insert(group.ports.begin(), group.ports.end());
    llvm::SmallVector<std::string> unusedOutputNames;
    for (unsigned i = 0; i < oldModel.getNumPorts(); ++i)
      if (oldModel.getPortDirection(i) == Direction::Out &&
          !mlir::isa<ClockType>(oldModel.getPorts()[i].type) &&
          !channelOutputPorts.count(i))
        unusedOutputNames.push_back(oldModel.getPortName(i).str());
    auto firstInstance = selectedPorts.front()->binding->instance;
    std::string modelInstanceName = firstInstance.getName().str();
    struct SelectedOutput {
      const goldengate::FAMETopChannelPort *port;
      llvm::SmallVector<std::string> modelPortNames;
      goldengate::LocalChannelDependency dependency;
    };
    llvm::SmallVector<SelectedOutput> selectedOutputs;
    if (rewriteInputsWithOutput) {
      llvm::SmallVector<llvm::StringRef> outputNames;
      if (llvm::StringRef(argv[7]) == "all") {
        // Keep the output channel order from the model/channel analysis,
        // matching the sequence used by Scala's FAME module transformer.
        for (const auto &port : famePlan->sources)
          if (port.binding->portGroup->module == oldModel &&
              port.binding->instance == firstInstance) {
            if (port.binding->instancePorts.empty())
              return fail("FAME output channel has no model ports: " +
                          port.binding->globalName);
            outputNames.push_back(port.binding->globalName);
          }
      } else {
        llvm::StringRef(argv[7]).split(outputNames, ',', -1, true);
      }
      if (outputNames.empty())
        return fail("FAME model has no output channels");
      std::set<std::string> seenOutputs;
      auto model = mlir::dyn_cast<FModuleOp>(oldModel.getOperation());
      auto dependencies = goldengate::analyzeLocalChannelDependencies(
          model, typedBindings);
      for (llvm::StringRef name : outputNames) {
        if (name.empty() || !seenOutputs.insert(name.str()).second)
          return fail("FAME output names must be nonempty and unique");
        auto found = llvm::find_if(famePlan->sources, [&](const auto &port) {
          return port.binding->globalName == name;
        });
        if (found == famePlan->sources.end() ||
            found->binding->instancePorts.empty() ||
            found->binding->portGroup->module != oldModel ||
            found->binding->instance != firstInstance)
          return fail("selected FAME output is not on the input model: " +
                      name.str());
        auto dependency = llvm::find_if(dependencies, [&](const auto &entry) {
          return entry.outputChannel == found->binding->portGroup->name;
        });
        if (dependency == dependencies.end() ||
            !dependency->unresolvedPorts.empty() ||
            !dependency->unresolvedCauses.empty())
          return fail("selected FAME output has unresolved input dependencies: " +
                      name.str());
        SelectedOutput selected{&*found, {}, *dependency};
        for (unsigned modelPort : found->binding->instancePorts)
          selected.modelPortNames.push_back(
              oldModel.getPortName(modelPort).str());
        selectedOutputs.push_back(std::move(selected));
      }
    }
    for (const auto *selected : selectedPorts) {
      auto candidateInstance = selected->binding->instance;
      if (selected->binding->portGroup->module != oldModel ||
          candidateInstance.getName() != modelInstanceName)
        return fail("FAME input channels must belong to one model instance");
    }
    std::map<std::string, llvm::SmallVector<std::string>> originalPortNames;
    for (const auto *selected : selectedPorts)
      for (unsigned port : selected->binding->instancePorts)
        originalPortNames[selected->binding->globalName].push_back(
            oldModel.getPortName(port).str());
    auto targetClock = llvm::find_if(analyzedChannels, [](const auto &channel) {
      return channel.kind == goldengate::ChannelKind::TargetClock;
    });
    if (targetClock == analyzedChannels.end())
      return fail("FAME input channel has no target clock channel");
    for (const auto *selected : selectedPorts)
      if (selected->binding->globalName == targetClock->name)
        return fail("target clock channel is not a data input channel");
    auto clockPort = llvm::find_if(famePlan->sinks, [&](const auto &port) {
      return port.binding->globalName == targetClock->name &&
             port.binding->portGroup->module == oldModel;
    });
    if (clockPort == famePlan->sinks.end() ||
        clockPort->binding->instancePorts.size() != 1)
      return fail("FAME input channel has no single model clock token");
    std::string clockGlobalName = targetClock->name;
    std::string clockLocalName = clockPort->binding->portGroup->name;
    std::string clockModelPortName = oldModel.getPortName(
        clockPort->binding->instancePorts.front()).str();
    std::string clockTopPortName;
    for (const auto &connection : hierarchy->connections)
      if (connection.instance == clockPort->binding->instance &&
          connection.instancePort == clockPort->binding->instancePorts.front())
        clockTopPortName = hierarchy->top.getPortName(connection.topPort).str();
    if (clockTopPortName.empty())
      return fail("FAME input channel has no old top clock port");
    auto clockInstance = clockPort->binding->instance;
    std::string clockInstanceName = clockInstance.getName().str();
    std::string clockTopChannelName = clockPort->portName;
    std::string clockModelChannelName = clockLocalName + "_sink";
    BundleType clockChannelType = clockPort->type;
    std::string targetPrefix = "~" + circuit.getName().str() + "|";
    std::string rewriteError;
    auto retarget = [&](Annotation &annotation, llvm::StringRef member,
                        const std::string &from, const std::string &to) {
      auto entries = annotation.getMember<mlir::ArrayAttr>(member);
      if (!entries)
        return 0u;
      llvm::SmallVector<mlir::Attribute> updated;
      unsigned changed = 0;
      for (auto entry : entries) {
        if (auto value = mlir::dyn_cast<mlir::StringAttr>(entry);
            value && value.getValue() == from) {
          updated.push_back(mlir::StringAttr::get(&context, to));
          ++changed;
        } else {
          updated.push_back(entry);
        }
      }
      if (changed)
        annotation.setMember(member, mlir::ArrayAttr::get(&context, updated));
      return changed;
    };
    if (llvm::StringRef(argv[7]) == "all" && rewriteInputsWithOutput) {
      auto model = mlir::dyn_cast<FModuleOp>(oldModel.getOperation());
      if (mlir::failed(goldengate::consumeFAMEModelDontTouches(
              circuit, {model}, rewriteError)))
        return fail("FAME model DontTouch consumption: " + rewriteError);
      if (mlir::failed(goldengate::removeFAMEAncillaryTopClockConnects(
              hierarchy->top, firstInstance, rewriteError)))
        return fail("FAME ancillary top clock removal: " + rewriteError);
    }
    llvm::SmallVector<std::string> channelNames;
    for (const auto *selected : selectedPorts) {
      // Each port replacement clones the instance. Rebuild the hierarchy so
      // the next channel is resolved against live FIRRTL operations.
      std::string hierarchyError;
      auto currentHierarchy =
          goldengate::analyzeTopHierarchy(circuit, hierarchyError);
      if (!currentHierarchy)
        return fail("FAME input channel hierarchy: " + hierarchyError);
      auto binding = *selected->binding;
      binding.instance = {};
      for (auto candidate : currentHierarchy->top.getOps<InstanceOp>())
        if (candidate.getName() == modelInstanceName &&
            candidate.getModuleName() == oldModel.getModuleName()) {
          if (binding.instance)
            return fail("FAME model instance is not unique");
          binding.instance = candidate;
        }
      if (!binding.instance)
        return fail("FAME model instance disappeared during input rewrite");
      binding.instancePorts.clear();
      for (const auto &name : originalPortNames.at(binding.globalName)) {
        bool found = false;
        for (unsigned i = 0; i < oldModel.getNumPorts(); ++i)
          if (oldModel.getPortName(i) == name) {
            binding.instancePorts.push_back(i);
            found = true;
            break;
          }
        if (!found)
          return fail("FAME model input port disappeared: " + name);
      }
      auto port = *selected;
      port.binding = &binding;
      struct TargetRename {
        std::string oldTop, newTop, oldModel, newModel;
      };
      llvm::SmallVector<TargetRename> renames;
      auto removeCommonPrefix = [](llvm::StringRef name,
                                   llvm::StringRef channel) {
        while (!name.empty() && !channel.empty() &&
               name.front() == channel.front()) {
          name = name.drop_front();
          channel = channel.drop_front();
        }
        return name;
      };
      for (unsigned modelPort : binding.instancePorts) {
        if (binding.instance.getPortNameStr(modelPort) !=
            oldModel.getPortName(modelPort))
          return fail("FAME model input port identity changed");
        std::string oldModelName = oldModel.getPortName(modelPort).str();
        std::string oldTopName;
        for (const auto &connection : currentHierarchy->connections)
          if (connection.instance == binding.instance &&
              connection.instancePort == modelPort)
            oldTopName =
                currentHierarchy->top.getPortName(connection.topPort).str();
        if (oldTopName.empty())
          return fail("FAME input channel has no old top port");
        std::string topSuffix, modelSuffix;
        if (binding.instancePorts.size() > 1) {
          topSuffix =
              ("." + removeCommonPrefix(oldTopName, binding.globalName)).str();
          modelSuffix = ("." + removeCommonPrefix(
                                   oldModelName, binding.portGroup->name))
                            .str();
        }
        renames.push_back({
            targetPrefix + currentHierarchy->top.getName().str() + ">" + oldTopName,
            targetPrefix + currentHierarchy->top.getName().str() + ">" +
                port.portName + ".bits" + topSuffix,
            targetPrefix + oldModel.getModuleName().str() + ">" + oldModelName,
            targetPrefix + oldModel.getModuleName().str() + ">" +
                binding.portGroup->name + "_sink.bits" + modelSuffix});
      }
      if (mlir::failed(goldengate::rewriteFAMEInputChannel(
              *currentHierarchy, port, rewriteError)))
        return fail("FAME input channel rewrite: " + rewriteError);
      for (const auto &rename : renames)
        if (mlir::failed(goldengate::transferFAMEWrapperDontTouch(
                circuit, rename.oldTop, rename.newTop, rewriteError)))
          return fail("FAME wrapper DontTouch transfer: " + rewriteError);
      // SFC's RenameMap moves each selected channel leaf to its payload
      // field. Retain unrelated bridge and channel annotations.
      llvm::SmallVector<mlir::Attribute> updatedAnnotations;
      unsigned topTargets = 0, modelTargets = 0, readySinkTargets = 0;
      unsigned validSinkTargets = 0;
      for (auto attr : circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations")) {
        Annotation annotation(attr);
        auto globalName = annotation.getMember<mlir::StringAttr>("globalName");
        auto localName = annotation.getMember<mlir::StringAttr>("localName");
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          if (globalName && globalName.getValue() == binding.globalName)
            for (const auto &rename : renames)
              topTargets += retarget(annotation, "sinks", rename.oldTop,
                                     rename.newTop);
          // Handshake metadata can name a leaf of this channel in another
          // annotation, such as readySink or validSink.
          if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
            for (const auto &rename : renames) {
              mlir::NamedAttrList updatedInfo(info);
              if (auto readySink = info.getAs<mlir::StringAttr>("readySink");
                  readySink && readySink.getValue() == rename.oldTop) {
                updatedInfo.set(
                    "readySink", mlir::StringAttr::get(&context, rename.newTop));
                ++readySinkTargets;
              }
              if (auto validSink = info.getAs<mlir::StringAttr>("validSink");
                  validSink && validSink.getValue() == rename.oldTop) {
                updatedInfo.set(
                    "validSink", mlir::StringAttr::get(&context, rename.newTop));
                ++validSinkTargets;
              }
              info = updatedInfo.getDictionary(&context);
            }
            annotation.setMember("channelInfo", info);
          }
        }
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts) &&
            localName && localName.getValue() == binding.portGroup->name)
          for (const auto &rename : renames)
            modelTargets += retarget(annotation, "ports", rename.oldModel,
                                     rename.newModel);
        updatedAnnotations.push_back(annotation.getAttr());
      }
      auto channelInfo = llvm::find_if(analyzedChannels, [&](const auto &entry) {
        return entry.name == binding.globalName;
      });
      if (channelInfo == analyzedChannels.end() ||
          topTargets != renames.size() || modelTargets != renames.size() ||
          readySinkTargets !=
              (channelInfo->kind == goldengate::ChannelKind::DecoupledReverse) ||
          validSinkTargets !=
              (channelInfo->kind == goldengate::ChannelKind::DecoupledForward))
        return fail("FAME input channel annotation targets were not unique");
      circuit->setAttr("rawAnnotations",
                       mlir::ArrayAttr::get(&context, updatedAnnotations));
      channelNames.push_back(binding.portGroup->name);
    }
    llvm::SmallVector<std::string> outputNames;
    llvm::SmallVector<goldengate::LocalChannelDependency> outputDependencies;
    for (const auto &selectedOutput : selectedOutputs) {
      std::string hierarchyError;
      auto currentHierarchy =
          goldengate::analyzeTopHierarchy(circuit, hierarchyError);
      if (!currentHierarchy)
        return fail("FAME output channel hierarchy: " + hierarchyError);
      auto binding = *selectedOutput.port->binding;
      binding.instance = {};
      for (auto candidate : currentHierarchy->top.getOps<InstanceOp>())
        if (candidate.getName() == modelInstanceName &&
            candidate.getModuleName() == oldModel.getModuleName()) {
          if (binding.instance)
            return fail("FAME output model instance is not unique");
          binding.instance = candidate;
        }
      if (!binding.instance)
        return fail("FAME output model instance disappeared");
      binding.instancePorts.clear();
      for (const auto &name : selectedOutput.modelPortNames) {
        bool found = false;
        for (unsigned i = 0; i < oldModel.getNumPorts(); ++i)
          if (oldModel.getPortName(i) == name) {
            binding.instancePorts.push_back(i);
            found = true;
            break;
          }
        if (!found)
          return fail("FAME output model port disappeared: " + name);
      }
      auto port = *selectedOutput.port;
      port.binding = &binding;
      struct TargetRename {
        std::string oldTop, newTop, oldModel, newModel;
      };
      llvm::SmallVector<TargetRename> renames;
      auto removeCommonPrefix = [](llvm::StringRef name,
                                   llvm::StringRef channel) {
        while (!name.empty() && !channel.empty() &&
               name.front() == channel.front()) {
          name = name.drop_front();
          channel = channel.drop_front();
        }
        return name;
      };
      for (unsigned modelPort : binding.instancePorts) {
        std::optional<unsigned> topPort;
        for (const auto &connection : currentHierarchy->connections)
          if (connection.instance == binding.instance &&
              connection.instancePort == modelPort) {
            if (topPort)
              return fail("FAME output channel has multiple top connections");
            topPort = connection.topPort;
          }
        if (!topPort)
          return fail("FAME output channel lost a top port");
        auto oldTopName = currentHierarchy->top.getPortName(*topPort);
        auto oldModelName = oldModel.getPortName(modelPort);
        std::string topSuffix, modelSuffix;
        if (binding.instancePorts.size() > 1) {
          topSuffix = ("." + removeCommonPrefix(oldTopName,
                                                  binding.globalName)).str();
          modelSuffix = ("." + removeCommonPrefix(
                                   oldModelName, binding.portGroup->name)).str();
        }
        renames.push_back({
            targetPrefix + currentHierarchy->top.getName().str() + ">" +
                oldTopName.str(),
            targetPrefix + currentHierarchy->top.getName().str() + ">" +
                port.portName + ".bits" + topSuffix,
            targetPrefix + oldModel.getModuleName().str() + ">" +
                oldModelName.str(),
            targetPrefix + oldModel.getModuleName().str() + ">" +
                binding.portGroup->name + "_source.bits" + modelSuffix});
      }
      if (mlir::failed(goldengate::rewriteFAMEOutputChannel(
              *currentHierarchy, port, rewriteError)))
        return fail("FAME output channel rewrite: " + rewriteError);
      for (const auto &rename : renames)
        if (mlir::failed(goldengate::transferFAMEWrapperDontTouch(
                circuit, rename.oldTop, rename.newTop, rewriteError)))
          return fail("FAME wrapper DontTouch transfer: " + rewriteError);
      llvm::SmallVector<mlir::Attribute> updatedAnnotations;
      unsigned topTargets = 0, modelTargets = 0;
      for (auto attr : circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations")) {
        Annotation annotation(attr);
        auto globalName = annotation.getMember<mlir::StringAttr>("globalName");
        auto localName = annotation.getMember<mlir::StringAttr>("localName");
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
          if (globalName && globalName.getValue() == binding.globalName)
            for (const auto &rename : renames)
              topTargets += retarget(annotation, "sources", rename.oldTop,
                                     rename.newTop);
          if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
            mlir::NamedAttrList updated(info);
            for (const auto &rename : renames)
              for (llvm::StringRef field : {"readySource", "validSource"})
                if (auto value = info.getAs<mlir::StringAttr>(field);
                    value && value.getValue() == rename.oldTop)
                  updated.set(field, mlir::StringAttr::get(
                                         &context, rename.newTop));
            annotation.setMember("channelInfo", updated.getDictionary(&context));
          }
        }
        if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts) &&
            localName && localName.getValue() == binding.portGroup->name)
          for (const auto &rename : renames)
            modelTargets += retarget(annotation, "ports", rename.oldModel,
                                     rename.newModel);
        updatedAnnotations.push_back(annotation.getAttr());
      }
      if (topTargets != renames.size() || modelTargets != renames.size())
        return fail("FAME output annotation targets were not unique");
      circuit->setAttr("rawAnnotations",
                       mlir::ArrayAttr::get(&context, updatedAnnotations));
      outputNames.push_back(binding.portGroup->name);
      outputDependencies.push_back(selectedOutput.dependency);
    }
    auto model = mlir::dyn_cast<FModuleOp>(oldModel.getOperation());
    if (mlir::failed(goldengate::addFAMEHostControl(circuit, model,
                                                   rewriteError)))
      return fail("FAME host-control construction: " + rewriteError);
    llvm::SmallVector<goldengate::FAMEFiredChannel> firedChannels;
    for (const auto &name : channelNames)
      firedChannels.push_back({name, true, {}});
    for (const auto &name : outputNames)
      firedChannels.push_back({name, false, {}});
    if (mlir::failed(goldengate::ensureFAMEFiredRegisters(
            model, firedChannels, rewriteError)))
      return fail("FAME fired-register construction: " + rewriteError);
    if (mlir::failed(goldengate::rewriteFAMEInputReadies(
            model, channelNames, rewriteError)))
      return fail("FAME input-ready construction: " + rewriteError);
    if (!outputDependencies.empty() &&
        mlir::failed(goldengate::rewriteFAMEOutputValids(
            model, outputDependencies, rewriteError)))
      return fail("FAME output-valid construction: " + rewriteError);
    if (mlir::failed(goldengate::addFAMEClockChannelToken(
            hierarchy->top, model, clockInstanceName, clockTopPortName,
            clockModelPortName, clockTopChannelName, clockModelChannelName,
            clockChannelType, rewriteError)))
      return fail("FAME target clock token construction: " + rewriteError);
    std::string oldClockTopTarget = targetPrefix + hierarchy->top.getName().str() +
                                    ">" + clockTopPortName;
    std::string newClockTopTarget = targetPrefix + hierarchy->top.getName().str() +
                                    ">" + clockTopChannelName + ".bits";
    std::string oldClockModelTarget = targetPrefix + model.getName().str() +
                                      ">" + clockModelPortName;
    std::string newClockModelTarget = targetPrefix + model.getName().str() +
                                      ">" + clockModelChannelName + ".bits";
    llvm::SmallVector<mlir::Attribute> clockAnnotations;
    unsigned clockTopTargets = 0, clockModelTargets = 0;
    for (auto attr : circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attr);
      auto globalName = annotation.getMember<mlir::StringAttr>("globalName");
      auto localName = annotation.getMember<mlir::StringAttr>("localName");
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection) &&
          globalName && globalName.getValue() == clockGlobalName)
        clockTopTargets += retarget(annotation, "sinks", oldClockTopTarget,
                                    newClockTopTarget);
      if (annotation.isClass(goldengate::AnnotationClasses::ChannelPorts) &&
          localName && localName.getValue() == clockLocalName)
        clockModelTargets += retarget(annotation, "ports", oldClockModelTarget,
                                      newClockModelTarget);
      clockAnnotations.push_back(annotation.getAttr());
    }
    if (clockTopTargets != 1 || clockModelTargets != 1)
      return fail("FAME target clock token annotation targets were not unique");
    circuit->setAttr("rawAnnotations",
                     mlir::ArrayAttr::get(&context, clockAnnotations));
    mlir::Value clockToken = model.getBodyBlock()->getArgument(model.getNumPorts() - 1);
    mlir::OpBuilder tokenBuilder(&model.getBodyBlock()->front());
    mlir::Value clockBits = tokenBuilder.create<SubfieldOp>(model.getLoc(),
                                                             clockToken, "bits");
    mlir::Value clockEnable =
        tokenBuilder.create<AsUIntPrimOp>(model.getLoc(), clockBits).getResult();
    for (auto &channel : firedChannels)
      channel.clockDomainEnable = clockEnable;
    if (mlir::failed(goldengate::addFAMEClockEnable(
            model, clockModelPortName, clockEnable, rewriteError)))
      return fail("FAME target clock enable construction: " + rewriteError);
    if (!outputNames.empty()) {
      // The Scala FAME clock metadata uses the buffered clock enable for
      // outputs, while input channels use the incoming clock token.
      RegResetOp outputClockEnableReg;
      model.walk([&](RegResetOp reg) {
        if (reg.getName() == clockModelPortName + "_enabled")
          outputClockEnableReg = reg;
      });
      if (!outputClockEnableReg)
        return fail("FAME output clock enable register is missing");
      // FAME1's output-channel clock flag is asUInt(buffered enable).  Keep
      // that operation explicit even when this single-clock enable is already
      // UInt<1>, so the generated fired-state rules retain SFC's clock cast.
      mlir::OpBuilder enableBuilder(outputClockEnableReg);
      enableBuilder.setInsertionPointAfter(outputClockEnableReg);
      mlir::Value outputClockEnable =
          enableBuilder
              .create<AsUIntPrimOp>(model.getLoc(),
                                    outputClockEnableReg.getResult())
              .getResult();
      for (auto &channel : firedChannels)
        if (!channel.isInput)
          channel.clockDomainEnable = outputClockEnable;
    }
    if (mlir::failed(goldengate::rewriteFAMEFiredStates(
            model, firedChannels, rewriteError)))
      return fail("FAME fired-state construction: " + rewriteError);
    if (mlir::failed(goldengate::addFAMEClockGate(
            circuit, model, clockModelPortName, rewriteError)))
      return fail("FAME target clock gate construction: " + rewriteError);
    if (mlir::failed(goldengate::removeFAMETargetClockPort(
            hierarchy->top, model, clockInstanceName, clockTopPortName,
            clockModelPortName, rewriteError)))
      return fail("FAME target clock port removal: " + rewriteError);
    if (mlir::failed(goldengate::internalizeFAMEOutputClocks(
            hierarchy->top, model, clockInstanceName, rewriteError)))
      return fail("FAME output clock internalization: " + rewriteError);
    llvm::SmallVector<llvm::StringRef> unusedOutputs;
    for (const auto &name : unusedOutputNames)
      unusedOutputs.push_back(name);
    if (mlir::failed(goldengate::internalizeFAMEUnusedOutputs(
            circuit, model, unusedOutputs, rewriteError)))
      return fail("FAME unused output internalization: " + rewriteError);
    // This is a partial boundary until every model output is channelized.
    if (mlir::failed(goldengate::rewriteFAMEFinishing(
            model, channelNames, outputNames, clockLocalName, rewriteError)))
      return fail("FAME partial finishing construction: " + rewriteError);
    if (mlir::failed(goldengate::groupFAMEChannelPorts(
            hierarchy->top, model, clockInstanceName,
            clockModelPortName + "_sink", rewriteError)))
      return fail("FAME channel port grouping: " + rewriteError);
    if (llvm::StringRef(argv[7]) == "all" && rewriteInputsWithOutput) {
      if (mlir::failed(goldengate::removeFAMEStaleTopClocks(
              hierarchy->top, rewriteError)))
        return fail("FAME stale top clock removal: " + rewriteError);
      llvm::SmallVector<llvm::StringRef> retainedTopPorts;
      for (const auto &name : retainedTopPortNames)
        retainedTopPorts.push_back(name);
      llvm::SmallVector<llvm::StringRef> orderedTopPorts;
      for (const auto &port : famePlan->sinks)
        orderedTopPorts.push_back(port.portName);
      for (const auto &port : famePlan->sources)
        orderedTopPorts.push_back(port.portName);
      if (mlir::failed(goldengate::orderFAMETopPorts(
              hierarchy->top, retainedTopPorts, orderedTopPorts, rewriteError)))
        return fail("FAME top port ordering: " + rewriteError);
    }
    if (mlir::failed(mlir::verify(*module)))
      return fail("FAME input channel rewrite produced invalid FIRRTL IR");
    // Keep an executable FIRRTL boundary alongside the MLIR checkpoint.  It
    // lets the transformed CIRCT operations be compared directly with SFC's
    // post-fame-transform.fir, before any downstream RTL lowering.
    llvm::SmallString<256> firPath(outputDir);
    llvm::sys::path::append(firPath, "post-fame-transform.fir");
    std::error_code firError;
    llvm::raw_fd_ostream firOut(firPath, firError);
    if (firError)
      return fail("cannot write FAME FIRRTL: " + firError.message());
    if (mlir::failed(exportFIRFile(*module, firOut, std::nullopt,
                                  exportFIRVersion)))
      return fail("cannot export FAME FIRRTL");
    llvm::SmallString<256> annotationPath(outputDir);
    llvm::sys::path::append(annotationPath, "post-fame-transform.json");
    if (mlir::failed(goldengate::emitFAMEAnnotations(circuit, annotationPath,
                                                     rewriteError)))
      return fail("FAME annotation emission: " + rewriteError);
    llvm::sys::path::remove_filename(annotationPath);
    llvm::sys::path::append(annotationPath, "post-fame-transform-all.json");
    if (mlir::failed(goldengate::emitAllAnnotations(circuit, annotationPath,
                                                    rewriteError)))
      return fail("FAME retained annotation emission: " + rewriteError);
    llvm::SmallString<256> rewrittenPath(outputDir);
    llvm::sys::path::append(rewrittenPath, !outputNames.empty()
                                              ? "input-output-channel.mlir"
                                              : "input-channel.mlir");
    std::error_code writeError;
    llvm::raw_fd_ostream rewrittenOut(rewrittenPath, writeError);
    if (writeError)
      return fail("cannot write input-channel MLIR: " + writeError.message());
    module->print(rewrittenOut);
    rewrittenOut << '\n';
    llvm::outs() << "Rewrote FAME channel ports and controls in "
                 << rewrittenPath << '\n';
    return 0;
  }
  llvm::json::Array fameSinks, fameSources, staleTopPorts, retainedTopPorts;
  auto typeName = [](BundleType type) {
    std::string spelling;
    llvm::raw_string_ostream out(spelling);
    type.print(out);
    return spelling;
  };
  for (const auto &port : famePlan->sinks)
    fameSinks.push_back(llvm::json::Object{
        {"global_name", port.binding->globalName}, {"port", port.portName},
        {"type", typeName(port.type)}});
  for (const auto &port : famePlan->sources)
    fameSources.push_back(llvm::json::Object{
        {"global_name", port.binding->globalName}, {"port", port.portName},
        {"type", typeName(port.type)}});
  for (unsigned port : famePlan->staleTopPorts)
    staleTopPorts.push_back(hierarchy->top.getPortName(port).str());
  for (unsigned i = 0, n = hierarchy->top.getPorts().size(); i != n; ++i)
    if (!llvm::is_contained(famePlan->staleTopPorts, i))
      retainedTopPorts.push_back(hierarchy->top.getPortName(i).str());
  llvm::json::Object fameTopPortPlan{
      {"sinks", std::move(fameSinks)},
      {"sources", std::move(fameSources)},
      {"stale_top_ports", std::move(staleTopPorts)},
      {"retained_top_ports", std::move(retainedTopPorts)}};

  llvm::json::Array localChannelDependencies;
  for (auto model : transformedModelOps) {
    auto internal = mlir::dyn_cast<FModuleOp>(model.getOperation());
    if (!internal)
      return fail("FAME transform targets an external module");
    for (auto &dependency :
         goldengate::analyzeLocalChannelDependencies(internal, typedBindings)) {
      llvm::json::Array inputs, unresolved, causes;
      for (const auto &name : dependency.inputChannels)
        inputs.push_back(name);
      for (const auto &name : dependency.unresolvedPorts)
        unresolved.push_back(name);
      for (const auto &cause : dependency.unresolvedCauses)
        causes.push_back(cause);
      localChannelDependencies.push_back(llvm::json::Object{
          {"module", internal.getName().str()},
          {"output_channel", dependency.outputChannel},
          {"input_channels", std::move(inputs)},
          {"unresolved_ports", std::move(unresolved)},
          {"unresolved_causes", std::move(causes)}});
    }
  }

  llvm::json::Array modules;
  unsigned moduleCount = 0, extmoduleCount = 0, portCount = 0;
  unsigned instanceCount = 0;
  for (auto &op : circuit.getBodyBlock()->getOperations()) {
    auto mod = mlir::dyn_cast<FModuleLike>(&op);
    if (!mod)
      continue;
    bool external = mlir::isa<FExtModuleOp>(&op);
    external ? ++extmoduleCount : ++moduleCount;
    llvm::json::Array ports;
    for (auto port : mod.getPorts()) {
      ports.push_back(port.name.getValue().str());
      ++portCount;
    }
    llvm::json::Array instances;
    op.walk([&](InstanceOp instance) {
      instances.push_back(llvm::json::Object{
          {"name", instance.getName().str()},
          {"module", instance.getModuleName().str()}});
      ++instanceCount;
    });
    modules.push_back(llvm::json::Object{
        {"name", mod.getModuleName().str()}, {"external", external},
        {"ports", std::move(ports)}, {"instances", std::move(instances)}});
  }

  llvm::json::Object annotationClasses;
  for (const auto &[name, count] : classes)
    annotationClasses[name] = count;
  llvm::json::Object report{
      {"circuit", circuit.getName().str()},
      {"module_count", moduleCount},
      {"extmodule_count", extmoduleCount},
      {"port_count", portCount},
      {"instance_count", instanceCount},
      {"annotation_count", rawAnnotations.size()},
      {"deduplicated_embedded_annotations", deduplicatedEmbeddedAnnotations},
      {"annotation_classes", std::move(annotationClasses)},
      {"bridge_instances", std::move(bridgeInstances)},
      {"channel_ports", std::move(channelPorts)},
      {"channel_connections", std::move(channelConnections)},
      {"top_connections", std::move(topConnections)},
      {"model_channel_bindings", std::move(modelChannelBindings)},
      {"fame_top_port_plan", std::move(fameTopPortPlan)},
      {"local_channel_dependencies", std::move(localChannelDependencies)},
      {"transformed_modules", std::move(transformedModules)},
      {"modules", std::move(modules)}};

  llvm::SmallString<256> irPath(outputDir);
  llvm::sys::path::append(irPath, "input.mlir");
  std::error_code error;
  llvm::raw_fd_ostream irOut(irPath, error);
  if (error)
    return fail("cannot write MLIR output: " + error.message());
  module->print(irOut);
  irOut << '\n';
  irOut.close();

  llvm::SmallString<256> reportPath(outputDir);
  llvm::sys::path::append(reportPath, "analysis.json");
  llvm::raw_fd_ostream reportOut(reportPath, error);
  if (error)
    return fail("cannot write analysis output: " + error.message());
  reportOut << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(report)));
  reportOut.close();
  llvm::outs() << "Imported " << firPath << " into " << irPath << '\n';
  return 0;
}
