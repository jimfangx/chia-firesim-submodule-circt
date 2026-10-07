// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include <set>

using namespace mlir;
using namespace circt::firrtl;

std::optional<llvm::SmallVector<goldengate::FAMEChannelClockDomain>>
goldengate::analyzeFAMEChannelClockDomains(
    CircuitOp circuit, FModuleOp model, ArrayRef<FAMEHubClockDomain> domains,
    std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!hierarchy || !raw || domains.empty()) {
    if (error.empty()) error = "channel clock domains need hierarchy, annotations and a hub";
    return std::nullopt;
  }
  std::set<unsigned> hubPorts;
  for (const auto &domain : domains) {
    if (domain.modelPort >= model.getNumPorts() ||
        model.getPortName(domain.modelPort) != domain.modelClockName ||
        model.getPortDirection(domain.modelPort) != Direction::In ||
        !isa<ClockType>(model.getPortType(domain.modelPort)) ||
        !hubPorts.insert(domain.modelPort).second) {
      error = "stale or duplicate FAME hub clock identity";
      return std::nullopt;
    }
  }
  SmallVector<ModelPortGroup> groups;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::ChannelPorts)) continue;
    auto group = analyzeModelPortGroup(circuit, anno, error);
    if (!group) return std::nullopt;
    groups.push_back(std::move(*group));
  }
  SmallVector<FAMEChannelClockDomain> result;
  std::set<std::string> globals;
  std::set<std::pair<std::string, Direction>> locals;
  std::set<const ModelPortGroup *> outputGroups;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::ChannelConnection)) continue;
    auto channel = analyzeChannelConnection(circuit, anno, error);
    if (!channel) return std::nullopt;
    if (!globals.insert(channel->name).second) {
      error = "duplicate channel clock global identity: " + channel->name;
      return std::nullopt;
    }
    if (channel->kind == ChannelKind::TargetClock) continue;
    auto bindings = bindChannelToModels(*channel, *hierarchy, groups, error);
    if (!bindings) return std::nullopt;
    for (const auto &binding : *bindings) {
      const auto &group = *binding.portGroup;
      if (group.module != model) continue;
      // Shared output branches use one local producer FSM and therefore one
      // domain assignment. bindChannelToModels checks each branch's clock.
      if (group.direction == Direction::Out &&
          !outputGroups.insert(binding.portGroup).second)
        continue;
      if (!locals.emplace(group.name, group.direction).second || !group.clockPort) {
        error = "missing or duplicate associated channel clock: " + group.name;
        return std::nullopt;
      }
      auto source = analyzeLocalChannelClockSource(model, *group.clockPort, error);
      if (!source) {
        error = "channel " + group.name + ": " + error;
        return std::nullopt;
      }
      if (!hubPorts.count(*source)) {
        error = "channel clock source is outside the FAME hub: " + group.name;
        return std::nullopt;
      }
      result.push_back({channel->name, group.name, group.direction,
                        model.getPortName(*source).str()});
    }
  }
  return result;
}
