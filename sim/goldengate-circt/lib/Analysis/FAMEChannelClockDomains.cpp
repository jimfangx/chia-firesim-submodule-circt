// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

std::optional<llvm::SmallVector<goldengate::FAMEChannelClockDomain>>
goldengate::analyzeFAMEChannelClockDomains(
    CircuitOp circuit, FModuleOp model, ArrayRef<FAMEHubClockDomain> domains,
    std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!hierarchy || !raw || !model ||
      model->getParentOp() != circuit.getOperation()) {
    if (error.empty()) error = "channel clock domains need hierarchy, annotations and a model";
    return std::nullopt;
  }
  // Empty domain metadata selects SFC's non-hub VirtualClockChannel. That
  // channel substitutes one for the sole target clock; it does not project a
  // remote hub's clock token into this independently scheduled model.
  bool virtualClock = domains.empty();
  std::set<unsigned> hubPorts;
  if (virtualClock) {
    for (unsigned i = 0; i < model.getNumPorts(); ++i)
      if (model.getPortDirection(i) == Direction::In &&
          isa<ClockType>(model.getPortType(i)))
        hubPorts.insert(i);
    if (hubPorts.size() != 1) {
      error = "non-hub FAME model requires one scalar target clock";
      return std::nullopt;
    }
  }
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
  struct LocalClock {
    const ModelPortGroup *group;
    std::string clockName;
    bool hasClockDomain;
    llvm::SmallVector<InstanceOp> instances;
  };
  std::map<std::pair<std::string, Direction>, LocalClock> locals;
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
      std::string clockName;
      if (group.clockPort) {
        auto source = analyzeLocalChannelClockSource(model, *group.clockPort, error);
        if (!source) {
          error = "channel " + group.name + ": " + error;
          return std::nullopt;
        }
        if (!hubPorts.count(*source)) {
          error = "channel clock source is outside the FAME hub: " + group.name;
          return std::nullopt;
        }
        clockName = model.getPortName(*source).str();
      } else if (!virtualClock) {
        error = "missing associated channel clock: " + group.name;
        return std::nullopt;
      }
      // A definition has one local FSM per group even when instantiated more
      // than once. Validate every global branch before sharing that FSM: each
      // branch still has to bind to its own instance and associated clock.
      auto [previous, inserted] = locals.emplace(
          std::make_pair(group.name, group.direction),
          LocalClock{binding.portGroup, clockName, bool(group.clockPort),
                     {binding.instance}});
      if (!inserted) {
        auto &local = previous->second;
        if (local.group != binding.portGroup || local.clockName != clockName ||
            local.hasClockDomain != bool(group.clockPort) ||
            (group.direction == Direction::In &&
             llvm::is_contained(local.instances, binding.instance))) {
          error = "incompatible shared FAME channel clock: " + group.name;
          return std::nullopt;
        }
        local.instances.push_back(binding.instance);
        continue;
      }
      result.push_back({channel->name, group.name, group.direction,
                        std::move(clockName), bool(group.clockPort)});
    }
  }
  return result;
}
