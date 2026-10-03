// See LICENSE for license details.
#include "goldengate/ModelAnalysis.h"
#include <algorithm>

using namespace circt::firrtl;

std::optional<goldengate::ModelPortGroup>
goldengate::analyzeModelPortGroup(CircuitOp circuit, Annotation annotation,
                                 std::string &error) {
  auto name = annotation.getMember<mlir::StringAttr>("localName");
  auto portTargets = annotation.getMember<mlir::ArrayAttr>("ports");
  if (!name || !portTargets || portTargets.empty()) {
    error = "FAMEChannelPortsAnnotation has no name or ports";
    return std::nullopt;
  }

  ModelPortGroup group;
  group.name = name.getValue().str();
  std::optional<Direction> direction;
  for (auto attr : portTargets) {
    auto spelling = mlir::dyn_cast<mlir::StringAttr>(attr);
    if (!spelling) {
      error = "channel port target is not a string";
      return std::nullopt;
    }
    auto target = resolveAnnotationTarget(circuit, spelling.getValue(), error);
    if (!target || !target->port) {
      if (error.empty()) error = "channel target is not a module port";
      return std::nullopt;
    }
    if (group.module && group.module != target->module) {
      error = "channel ports span more than one model module";
      return std::nullopt;
    }
    group.module = target->module;
    auto portDirection = group.module.getPortDirection(*target->port);
    if (direction && *direction != portDirection) {
      error = "channel ports have mixed directions";
      return std::nullopt;
    }
    direction = portDirection;
    group.ports.push_back(*target->port);
  }
  group.direction = *direction;
  if (auto clock = annotation.getMember<mlir::StringAttr>("clockPort")) {
    auto target = resolveAnnotationTarget(circuit, clock.getValue(), error);
    if (!target || !target->port || target->module != group.module) {
      if (error.empty()) error = "invalid channel clock port target";
      return std::nullopt;
    }
    group.clockPort = *target->port;
  }
  return group;
}

std::optional<llvm::SmallVector<goldengate::ModelChannelBinding>>
goldengate::bindChannelToModels(const GGChannelConnection &channel,
                                const TopHierarchy &hierarchy,
                                llvm::ArrayRef<ModelPortGroup> portGroups,
                                std::string &error) {
  struct InstancePorts {
    InstanceOp instance;
    llvm::SmallVector<unsigned> ports;
    bool hasSource = false;
    bool hasSink = false;
  };
  llvm::SmallVector<InstancePorts> instances;
  auto lookupConnection = [&](unsigned topPort)
      -> const TopPortConnection * {
    auto found = llvm::find_if(hierarchy.connections,
                              [&](const TopPortConnection &c) {
                                return c.topPort == topPort;
                              });
    return found == hierarchy.connections.end() ? nullptr : &*found;
  };
  auto addEndpoint = [&](const GGTarget &target, bool source) {
    if (target.module != hierarchy.top || !target.port) {
      error = "channel endpoint is not a top module port";
      return;
    }
    auto *connection = lookupConnection(*target.port);
    if (!connection)
      return; // A top-level loopback has no model instance on this side.
    auto existing = llvm::find_if(instances, [&](const InstancePorts &entry) {
      return entry.instance == connection->instance;
    });
    if (existing == instances.end()) {
      instances.push_back({connection->instance, {connection->instancePort},
                           source, !source});
    } else {
      existing->ports.push_back(connection->instancePort);
      existing->hasSource |= source;
      existing->hasSink |= !source;
    }
  };
  for (const auto &endpoint : channel.sources) addEndpoint(endpoint, true);
  for (const auto &endpoint : channel.sinks) addEndpoint(endpoint, false);
  if (!error.empty()) return std::nullopt;

  llvm::SmallVector<ModelChannelBinding> bindings;
  for (auto &entry : instances) {
    llvm::sort(entry.ports);
    if (std::adjacent_find(entry.ports.begin(), entry.ports.end()) !=
        entry.ports.end()) {
      error = "channel repeats a model port: " + channel.name;
      return std::nullopt;
    }
    const ModelPortGroup *match = nullptr;
    for (const auto &group : portGroups) {
      auto modelModule = group.module;
      if (modelModule.getModuleName() != entry.instance.getModuleName() ||
          group.ports.size() != entry.ports.size())
        continue;
      llvm::SmallVector<unsigned> sorted(group.ports);
      llvm::sort(sorted);
      if (sorted != entry.ports)
        continue;
      if (match) {
        error = "ambiguous local model port group for " + channel.name;
        return std::nullopt;
      }
      match = &group;
    }
    if (!match) {
      error = "no local model port group for " + channel.name;
      return std::nullopt;
    }
    if ((entry.hasSource && match->direction != Direction::Out) ||
        (entry.hasSink && match->direction != Direction::In)) {
      error = "model channel direction disagrees with global endpoints: " +
              channel.name;
      return std::nullopt;
    }
    if (channel.clock || match->clockPort) {
      const TopPortConnection *clockConnection = nullptr;
      if (channel.clock && channel.clock->port &&
          channel.clock->module == hierarchy.top)
        clockConnection = lookupConnection(*channel.clock->port);
      if (!match->clockPort || !clockConnection ||
          clockConnection->instance != entry.instance ||
          clockConnection->instancePort != *match->clockPort) {
        error = "model channel clock disagrees with global clock: " +
                channel.name;
        return std::nullopt;
      }
    }
    bindings.push_back({channel.name, match, entry.instance,
                        std::move(entry.ports)});
  }
  return bindings;
}
