// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include <set>

using namespace circt::firrtl;

std::optional<llvm::SmallVector<goldengate::FAMEOutputSelection>>
goldengate::analyzeFAMEOutputSelection(CircuitOp circuit, FModuleOp model,
                                       std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  if (!hierarchy)
    return std::nullopt;
  auto annotations = circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
  if (!annotations) {
    error = "FAME output selection needs retained channel annotations";
    return std::nullopt;
  }
  llvm::SmallVector<ModelPortGroup> groups;
  for (auto attr : annotations) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelPorts))
      continue;
    auto group = analyzeModelPortGroup(circuit, annotation, error);
    if (!group)
      return std::nullopt;
    groups.push_back(std::move(*group));
  }
  llvm::SmallVector<ModelChannelBinding> modelBindings;
  llvm::SmallVector<FAMEOutputSelection> outputs;
  std::set<std::string> globalNames, localOutputs;
  std::set<unsigned> boundPorts;
  for (auto attr : annotations) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto channel = analyzeChannelConnection(circuit, annotation, error);
    if (!channel)
      return std::nullopt;
    if (!globalNames.insert(channel->name).second) {
      error = "duplicate global FAME channel: " + channel->name;
      return std::nullopt;
    }
    if (channel->kind == ChannelKind::TargetClock)
      continue;
    auto bindings = bindChannelToModels(*channel, *hierarchy, groups, error);
    if (!bindings)
      return std::nullopt;
    for (const auto &binding : *bindings) {
      auto boundModule = binding.portGroup->module;
      if (boundModule.getOperation() != model.getOperation())
        continue;
      for (unsigned port : binding.instancePorts)
        if (!boundPorts.insert(port).second) {
          error = "two FAME channels claim model port: " +
                  model.getPortName(port).str();
          return std::nullopt;
        }
      modelBindings.push_back(binding);
      if (binding.portGroup->direction != Direction::Out)
        continue;
      if (!localOutputs.insert(binding.portGroup->name).second) {
        error = "duplicate local FAME output: " + binding.portGroup->name;
        return std::nullopt;
      }
      outputs.push_back({channel->name, binding.portGroup->name, channel->kind,
                         unsigned(channel->sources.size()), {}});
    }
  }
  auto dependencies = analyzeLocalChannelDependencies(model, modelBindings);
  for (auto &output : outputs) {
    auto dependency = llvm::find_if(dependencies, [&](const auto &entry) {
      return entry.outputChannel == output.localName;
    });
    if (dependency == dependencies.end()) {
      error = "no dependencies for FAME output: " + output.localName;
      return std::nullopt;
    }
    if (!dependency->unresolvedPorts.empty() ||
        !dependency->unresolvedCauses.empty()) {
      error = "FAME output " + output.localName +
              " has unresolved combinational dependencies:";
      for (const auto &port : dependency->unresolvedPorts)
        error += " port=" + port;
      for (const auto &cause : dependency->unresolvedCauses)
        error += " cause=" + cause;
      return std::nullopt;
    }
    output.dependency = *dependency;
  }
  return outputs;
}
