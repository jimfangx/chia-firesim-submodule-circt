// See LICENSE for license details.
// Oracle: MidasTransforms CSV through FAMETransform; FPGATop channel binding.
#include "goldengate/AutoCounterPipeline.h"
#include "goldengate/AutoCounterSynthesis.h"
#include "goldengate/AutoCounterBridge.h"
#include "goldengate/AutoCounterControl.h"
#include "goldengate/ChannelClockInfo.h"
#include "goldengate/ChannelExcision.h"
#include "goldengate/ExtractModel.h"
#include "goldengate/FAMEDefaults.h"
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/GlobalResetWiring.h"
#include "goldengate/InstrumentationControl.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/PrintWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/BuiltinOps.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/PromotePassthroughConnections.h"
#include "goldengate/TriggerWiring.h"
#include "goldengate/WrapTop.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/Verifier.h"
using namespace mlir;
using namespace circt::firrtl;
LogicalResult goldengate::compileAutoCounterCSVToBoundHosts(
    CircuitOp circuit, ArrayRef<StringRef> coverModules,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  AutoCounterPipelineResult next;
  auto stage = [&](StringRef name, auto function) -> LogicalResult {
    if (failed(function())) {
      error = name.str() + ": " + error;
      return failure();
    }
    return success();
  };
  if (failed(stage("AutoCounter CSV", [&] {
        return synthesizeEnabledAutoCounterCSV(*staged, coverModules, next.widgets, error);
      }))) return failure();
  if (!next.widgets) {
    error = "AutoCounter pipeline requires at least one selected CSV event";
    return failure();
  }
  if (failed(stage("TriggerWiring", [&] { return wireTriggers(*staged, next.triggerSources, error); })) ||
      failed(stage("GlobalResetConditionWiring", [&] { return wireGlobalReset(*staged, next.resetSinks, error); })) ||
      failed(stage("ChannelClockInfo", [&] { return analyzeChannelClocksAndUpdateBridges(*staged, error); })) ||
      failed(stage("WrapTop", [&] { return wrapTop(*staged, error); }))) return failure();
  unsigned promoted = 0;
  if (failed(stage("ExtractModel", [&] { return extractModels(*staged, promoted, error); })) ||
      failed(stage("PromotePassthroughConnections", [&] { return promotePassthroughConnections(*staged, promoted, error); })) ||
      failed(stage("FAMEDefaults", [&] { return addFAMEDefaults(*staged, error); })) ||
      failed(stage("FindDefaultClocks", [&] { return findDefaultClocks(*staged, error); })) ||
      failed(stage("ChannelExcision", [&] { return exciseChannels(*staged, error); })) ||
      failed(stage("InferModelPorts", [&] { return inferModelPorts(*staged, error); })) ||
      failed(stage("FAME model graph", [&] { return rewriteFAMEModelGraph(*staged, next.models, error, false); }))) return failure();
  SmallVector<FModuleOp> hosts;
  if (failed(stage("CSV host materialization", [&] { return materializeAutoCounterBridgeHosts(*staged, addressBits, idBits, hosts, error); }))) return failure();
  if (hosts.size() != next.widgets) {
    error = "AutoCounter pipeline constructor/host count changed during FAME";
    return failure();
  }
  if (failed(stage("CSV token binding", [&] { return bindAutoCounterBridgeHosts(*staged, hosts, error); }))) return failure();
  SmallVector<ControlMMIOWidget> widgets;
  if (failed(stage("CSV control catalog", [&] { return deriveAutoCounterControlMMIOCatalog(*staged, hosts, widgets, error); }))) return failure();
  if (widgets.size() != next.widgets) {
    error = "AutoCounter pipeline bound control catalog changed constructor count";
    return failure();
  }
  if (failed(verify(*staged))) {
    error = "AutoCounter pipeline produced invalid staged FIRRTL";
    return failure();
  }
  circuit->setAttrs((*staged)->getAttrs());
  auto &destination = circuit.getBodyBlock()->getOperations();
  destination.clear();
  destination.splice(destination.end(), staged->getBodyBlock()->getOperations());
  result = next;
  return success();
}

LogicalResult goldengate::compileAutoCounterCSVToControl(
    CircuitOp circuit, ArrayRef<StringRef> coverModules,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &header, std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  AutoCounterPipelineResult next;
  if(failed(compileAutoCounterCSVToBoundHosts(*staged,coverModules,addressBits,idBits,next,error)))return failure();
  SmallVector<FModuleOp> hosts;
  FModuleOp bound;
  for(auto module:staged->getOps<FModuleOp>())if(module.getName()==staged->getName())bound=module;
  auto bindings=bound?bound->getAttrOfType<ArrayAttr>("goldengate.autocounterHostBindings"):ArrayAttr();
  if(!bindings){error="CSV global pipeline is missing its host binding registry";return failure();}
  for(auto attr:bindings) {
    auto row=dyn_cast<DictionaryAttr>(attr);
    auto name=row?row.getAs<StringAttr>("hostModule"):StringAttr();
    FModuleOp host;
    if(name)for(auto module:staged->getOps<FModuleOp>())if(module.getName()==name)host=module;
    if(!host){error="CSV global pipeline is missing a constructor host";return failure();}
    hosts.push_back(host);
  }
  SmallVector<ControlMMIOWidget> widgets;
  SmallVector<ControlMMIORegion> regions;
  std::string nextHeader;
  if(failed(deriveAutoCounterControlMMIOCatalog(*staged,hosts,widgets,error))||
     failed(allocateControlMMIORegions(addressBits,widgets,regions,error))||
     failed(assembleAutoCounterControl(*staged,hosts,regions,nextHeader,error)))return failure();
  if(failed(verify(*staged))){error="CSV global pipeline produced invalid FIRRTL";return failure();}
  circuit->setAttrs((*staged)->getAttrs());
  auto &destination=circuit.getBodyBlock()->getOperations();
  destination.clear();
  destination.splice(destination.end(),staged->getBodyBlock()->getOperations());
  header=std::move(nextHeader);result=next;return success();
}


LogicalResult goldengate::compileInstrumentationSourceToControl(
    CircuitOp circuit, ArrayRef<StringRef> coverModules,
    unsigned addressBits, unsigned idBits, AutoCounterPipelineResult &result,
    std::string &header, std::string &decoderHeader, std::string &peerMap,
    std::string &error) {
  // LowerTypes owns a ModuleOp pass manager. Keep its entire work in a private
  // root so failure in normalization or late MMIO/stream assembly cannot leave
  // either target state or published driver collateral partially changed.
  OwningOpRef<ModuleOp> stagedRoot(ModuleOp::create(circuit.getLoc()));
  auto staged = cast<CircuitOp>(circuit->clone());
  stagedRoot->getBody()->push_back(staged);
  AutoCounterPipelineResult next;
  auto stage = [&](StringRef name, auto function) -> LogicalResult {
    if (failed(function())) {
      error = name.str() + ": " + error;
      return failure();
    }
    return success();
  };
  if (failed(stage("AutoCounter CSV", [&] {
        return synthesizeEnabledAutoCounterCSV(staged, coverModules, next.widgets, error);
      }))) return failure();
  if (!next.widgets) {
    error = "instrumentation source pipeline requires selected CSV events";
    return failure();
  }
  SmallVector<PrintStub> stubs;
  SmallVector<WiredPrint> routes;
  if (failed(stage("Print stubs", [&] { return synthesizePrintStubs(staged, stubs, error); })) ||
      failed(stage("Print hierarchy", [&] { return wirePrintStubsToTop(staged, stubs, routes, error); })) ||
      failed(stage("Print clocks", [&] { return completePrintClockWiring(staged, stubs, routes, error); })) ||
      failed(stage("Print synthesis", [&] { return completePrintSynthesis(staged, stubs, error); }))) return failure();
  next.printSites = routes.size();
  if (!next.printSites) {
    error = "instrumentation source pipeline requires selected enabled printf instances";
    return failure();
  }
  for (auto attr : staged->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    if (row && row.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
        row.getAs<StringAttr>("widgetClass") == AnnotationClasses::PrintBridgeModule)
      ++next.printWidgets;
  }
  if (failed(stage("TriggerWiring", [&] { return wireTriggers(staged, next.triggerSources, error); })) ||
      failed(stage("GlobalResetConditionWiring", [&] { return wireGlobalReset(staged, next.resetSinks, error); })) ||
      failed(stage("ChannelClockInfo", [&] { return analyzeChannelClocksAndUpdateBridges(staged, error); })) ||
      failed(stage("WrapTop", [&] { return wrapTop(staged, error); }))) return failure();
  unsigned promoted = 0;
  if (failed(stage("ExtractModel", [&] { return extractModels(staged, promoted, error); })) ||
      // PrintSynthesis exports typed bundles after the scalar source handoff.
      // Match SFC's post-ExtractModel lowering and transfer all retained targets.
      failed(stage("post-ExtractModel LowerTypes", [&] {
        return lowerTypesWithRetainedTargets(*stagedRoot, staged, error);
      })) ||
      failed(stage("PromotePassthroughConnections", [&] { return promotePassthroughConnections(staged, promoted, error); })) ||
      failed(stage("FAMEDefaults", [&] { return addFAMEDefaults(staged, error); })) ||
      failed(stage("FindDefaultClocks", [&] { return findDefaultClocks(staged, error); })) ||
      failed(stage("ChannelExcision", [&] { return exciseChannels(staged, error); })) ||
      failed(stage("InferModelPorts", [&] { return inferModelPorts(staged, error); })) ||
      failed(stage("FAME model graph", [&] { return rewriteFAMEModelGraph(staged, next.models, error, false); }))) return failure();
  std::string nextHeader, nextDecoder, nextPeer;
  if (failed(stage("instrumentation control/CPU transport", [&] {
        return compileInstrumentationControl(staged, addressBits, idBits,
            nextHeader, nextDecoder, nextPeer, error);
      }))) return failure();
  if (failed(verify(*stagedRoot))) {
    error = "instrumentation source pipeline produced invalid FIRRTL";
    return failure();
  }
  circuit->setAttrs(staged->getAttrs());
  auto &destination = circuit.getBodyBlock()->getOperations();
  destination.clear();
  destination.splice(destination.end(), staged.getBodyBlock()->getOperations());
  result = next;
  header = std::move(nextHeader);
  decoderHeader = std::move(nextDecoder);
  peerMap = std::move(nextPeer);
  return success();
}
