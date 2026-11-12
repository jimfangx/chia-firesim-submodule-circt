// See LICENSE for license details.
#include "goldengate/ModelAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <map>
#include <vector>

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

namespace {
// Matching uses unordered port sets, but keeps every candidate so duplicate
// groups still reject as ambiguous. Connection order is retained separately:
// binding historically selects the first edge, payload validation uses all.
struct BindingIndex {
  using Key = std::pair<std::string, std::vector<unsigned>>;
  llvm::DenseMap<unsigned, llvm::SmallVector<const goldengate::TopPortConnection *>> connections;
  std::map<Key, llvm::SmallVector<const goldengate::ModelPortGroup *>> groups;
  BindingIndex(const goldengate::TopHierarchy &hierarchy,
               llvm::ArrayRef<goldengate::ModelPortGroup> portGroups) {
    for (const auto &connection : hierarchy.connections)
      connections[connection.topPort].push_back(&connection);
    for (const auto &group : portGroups) {
      std::vector<unsigned> ports(group.ports.begin(), group.ports.end());
      llvm::sort(ports);
      auto module = group.module;
      groups[{module.getModuleName().str(), std::move(ports)}].push_back(&group);
    }
  }
  const goldengate::TopPortConnection *first(unsigned port) const {
    auto found = connections.find(port);
    return found == connections.end() ? nullptr : found->second.front();
  }
};

std::optional<llvm::SmallVector<goldengate::ModelChannelBinding>>
bindIndexed(const goldengate::GGChannelConnection &channel,
            const goldengate::TopHierarchy &hierarchy,
            const BindingIndex *index,
            llvm::ArrayRef<goldengate::ModelPortGroup> portGroups,
            std::string &error) {
  using namespace goldengate;
  struct InstancePorts {
    InstanceOp instance;
    llvm::SmallVector<unsigned> ports;
    bool hasSource = false;
    bool hasSink = false;
  };
  llvm::SmallVector<InstancePorts> instances;
  auto lookupConnection = [&](unsigned topPort) {
    if (index) return index->first(topPort);
    auto found = llvm::find_if(hierarchy.connections,
                              [&](const TopPortConnection &connection) {
                                return connection.topPort == topPort;
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
    if (index) {
      std::vector<unsigned> ports(entry.ports.begin(), entry.ports.end());
      auto found = index->groups.find({entry.instance.getModuleName().str(), std::move(ports)});
      if (found != index->groups.end()) {
        if (found->second.size() > 1) {
          error = "ambiguous local model port group for " + channel.name;
          return std::nullopt;
        }
        match = found->second.front();
      }
    } else {
      // Single standalone queries retain their original lazy scan; building a
      // full index for one channel would add work to unrelated callers.
      for (const auto &group : portGroups) {
        auto module = group.module;
        if (module.getModuleName() != entry.instance.getModuleName() ||
            group.ports.size() != entry.ports.size()) continue;
        llvm::SmallVector<unsigned> sorted(group.ports);
        llvm::sort(sorted);
        if (sorted != entry.ports) continue;
        if (match) {
          error = "ambiguous local model port group for " + channel.name;
          return std::nullopt;
        }
        match = &group;
      }
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
      // SFC projects the global clock into a local clockPort only on the
      // module exporting it. A promoted SRAM on the other end uses a virtual
      // clock channel; its group deliberately has no associated clockPort.
      // Do not accept a missing local annotation on the clock-owning model.
      bool localClock = clockConnection &&
          clockConnection->instance == entry.instance;
      auto clockInstance = clockConnection ? clockConnection->instance : InstanceOp();
      bool remoteClock = clockConnection &&
          clockInstance.getModuleName() != entry.instance.getModuleName();
      if (!clockConnection ||
          (match->clockPort ? (!localClock ||
              clockConnection->instancePort != *match->clockPort)
                           : !remoteClock)) {
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

} // namespace

std::optional<llvm::SmallVector<goldengate::ModelChannelBinding>>
goldengate::bindChannelToModels(const GGChannelConnection &channel,
                                const TopHierarchy &hierarchy,
                                llvm::ArrayRef<ModelPortGroup> portGroups,
                                std::string &error) {
  return bindIndexed(channel, hierarchy, nullptr, portGroups, error);
}

struct goldengate::PreparedFAMEAnalysis::Impl {
  CircuitOp circuit;
  TopHierarchy hierarchy;
  llvm::SmallVector<ModelPortGroup> groups;
  llvm::SmallVector<GGChannelConnection, 0> channels;
  std::unique_ptr<BindingIndex> index;
  std::vector<std::optional<llvm::SmallVector<ModelChannelBinding>>> bindings;
};

goldengate::PreparedFAMEAnalysis::PreparedFAMEAnalysis(std::unique_ptr<Impl> value)
    : impl(std::move(value)) {}
goldengate::PreparedFAMEAnalysis::~PreparedFAMEAnalysis() = default;

std::unique_ptr<goldengate::PreparedFAMEAnalysis>
goldengate::PreparedFAMEAnalysis::create(CircuitOp circuit, std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  if (!hierarchy) return nullptr;
  auto raw = circuit->getAttrOfType<mlir::ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "FAME analysis needs retained channel annotations";
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->circuit = circuit;
  impl->hierarchy = std::move(*hierarchy);
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelPorts)) continue;
    auto group = analyzeModelPortGroup(circuit, annotation, error);
    if (!group) return nullptr;
    impl->groups.push_back(std::move(*group));
  }
  for (auto attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection)) continue;
    auto channel = analyzeChannelConnection(circuit, annotation, error);
    if (!channel) return nullptr;
    impl->channels.push_back(std::move(*channel));
  }
  impl->index = std::make_unique<BindingIndex>(impl->hierarchy, impl->groups);
  impl->bindings.resize(impl->channels.size());
  return std::unique_ptr<PreparedFAMEAnalysis>(new PreparedFAMEAnalysis(std::move(impl)));
}

CircuitOp goldengate::PreparedFAMEAnalysis::circuit() const { return impl->circuit; }
const goldengate::TopHierarchy &goldengate::PreparedFAMEAnalysis::hierarchy() const { return impl->hierarchy; }
llvm::ArrayRef<goldengate::ModelPortGroup> goldengate::PreparedFAMEAnalysis::groups() const { return impl->groups; }
llvm::ArrayRef<goldengate::GGChannelConnection> goldengate::PreparedFAMEAnalysis::channels() const { return impl->channels; }
llvm::ArrayRef<const goldengate::TopPortConnection *>
goldengate::PreparedFAMEAnalysis::connectionsForTopPort(unsigned port) const {
  auto found = impl->index->connections.find(port);
  if (found == impl->index->connections.end()) return {};
  return found->second;
}
const llvm::SmallVector<goldengate::ModelChannelBinding> *
goldengate::PreparedFAMEAnalysis::bindingsFor(unsigned channel, std::string &error) {
  auto &cached = impl->bindings.at(channel);
  if (!cached) cached = bindIndexed(impl->channels[channel], impl->hierarchy, impl->index.get(), {}, error);
  return cached ? &*cached : nullptr;
}
