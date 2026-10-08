// See LICENSE for license details.
#include "goldengate/SRAMModelChannels.h"
#include "goldengate/ChannelExcision.h"
#include "goldengate/ExtractModel.h"
#include "goldengate/FAMEDefaults.h"
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/LabelSRAMModels.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/PromotePassthroughConnections.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Pass/PassManager.h"

using namespace circt::firrtl;
using namespace mlir;

/// Required input invariants: wrapped top, resolved last-connect semantics,
/// retained memory selections and a target-clock channel on the hub input.
/// Annotations consumed: FirrtlMemModelAnnotation, FirrtlFAMEModelAnnotation.
/// Annotations produced: SRAM port kinds, FAME model/channel/port annotations,
/// DontTouch annotations. IR mutations: wrap/promote selected memories, lower
/// aggregate ports, promote passthroughs, and excise inter-model connections.
/// Analyses required: native hierarchy and channel target resolution.
/// Analyses preserved: none (ports and hierarchy change).
/// Output invariants: verified ground FIRRTL, clocked inter-model channels,
/// module-local channel groups ready for FAME analysis. No RAM timing model or
/// FAME hardware rewrite is performed by this boundary.
LogicalResult goldengate::prepareSRAMModelChannels(
    ModuleOp root, CircuitOp circuit, unsigned &wrapped, unsigned &promoted,
    std::string &error) {
  wrapped = promoted = 0;
  unsigned passthroughs = 0;
  PassManager normalization(root.getContext());
  normalization.nest<CircuitOp>().addNestedPass<FModuleOp>(createLowerCHIRRTLPass());
  normalization.addNestedPass<CircuitOp>(createInferWidthsPass());
  normalization.addNestedPass<CircuitOp>(createInferResetsPass());
  if (failed(normalization.run(root))) {
    error = "SRAM normalization failed";
    return failure();
  }
  auto stage = [&](StringRef name, auto run) {
    if (failed(run())) {
      error = name.str() + ": " + error;
      return failure();
    }
    return success();
  };
  if (failed(stage("LabelSRAMModels", [&] {
        return labelSRAMModels(circuit, wrapped, error);
      })) ||
      failed(stage("ExtractModel", [&] {
        return extractModels(circuit, promoted, error);
      })) ||
      failed(stage("LowerTypes", [&] {
        return lowerTypesWithRetainedTargets(root, circuit, error);
      })) ||
      failed(stage("PromotePassthroughConnections", [&] {
        return promotePassthroughConnections(circuit, passthroughs, error);
      })) ||
      failed(stage("FAMEDefaults", [&] { return addFAMEDefaults(circuit, error); })) ||
      failed(stage("FindDefaultClocks", [&] { return findDefaultClocks(circuit, error); })) ||
      failed(stage("ChannelExcision", [&] { return exciseChannels(circuit, error); })) ||
      failed(stage("InferModelPorts", [&] { return inferModelPorts(circuit, error); })))
    return failure();
  if (failed(verify(root))) {
    error = "SRAM channel preparation produced invalid FIRRTL IR";
    return failure();
  }
  return success();
}
