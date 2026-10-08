// See LICENSE for license details.
#include "goldengate/SRAMModelChannels.h"
#include "goldengate/AnnotationClasses.h"
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

std::optional<llvm::SmallVector<goldengate::SRAMModelDependencies>>
goldengate::analyzeSRAMModelDependencies(CircuitOp circuit, std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!hierarchy || !raw) {
    if (error.empty()) error = "SRAM dependency analysis needs retained annotations";
    return std::nullopt;
  }
  SmallVector<ModelPortGroup> groups;
  SmallVector<FModuleOp> models;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::ChannelPorts)) {
      auto group = analyzeModelPortGroup(circuit, annotation, error);
      if (!group) return std::nullopt;
      groups.push_back(std::move(*group));
    } else if (annotation.isClass(AnnotationClasses::FAMETransform)) {
      auto spelling = annotation.getMember<StringAttr>("target");
      auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error)
                             : std::nullopt;
      auto model = target ? dyn_cast<FModuleOp>(target->module.getOperation()) : FModuleOp();
      if (!model || target->port) {
        if (error.empty()) error = "SRAM FAME target is not an internal module";
        return std::nullopt;
      }
      if (!llvm::is_contained(models, model)) models.push_back(model);
    }
  }
  SmallVector<ModelChannelBinding> bindings;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection)) continue;
    auto channel = analyzeChannelConnection(circuit, annotation, error);
    if (!channel) return std::nullopt;
    auto bound = bindChannelToModels(*channel, *hierarchy, groups, error);
    if (!bound) return std::nullopt;
    bindings.append(bound->begin(), bound->end());
  }
  SmallVector<SRAMModelDependencies> result;
  for (auto model : models) {
    auto outputs = analyzeLocalChannelDependencies(model, bindings);
    for (const auto &output : outputs) {
      if (!output.unresolvedPorts.empty() || !output.unresolvedCauses.empty()) {
        error = "SRAM model " + model.getName().str() + " output " +
                output.outputChannel + " has unresolved dependencies";
        for (const auto &cause : output.unresolvedCauses) error += ": " + cause;
        return std::nullopt;
      }
    }
    result.push_back({model, std::move(outputs)});
  }
  return result;
}
