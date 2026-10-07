// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include <set>

using namespace circt::firrtl;

std::optional<llvm::SmallVector<goldengate::FAMEHubClockDomain>>
goldengate::analyzeFAMEHubClockDomains(
    const GGChannelConnection &channel, const TopHierarchy &hierarchy,
    const ModelChannelBinding &binding, std::string &error) {
  auto reject = [&](llvm::StringRef reason)
      -> std::optional<llvm::SmallVector<FAMEHubClockDomain>> {
    error = "FAME hub clock domains: " + reason.str();
    return std::nullopt;
  };
  if (!binding.portGroup || !binding.instance || !hierarchy.top)
    return reject("missing model binding or top hierarchy");
  const auto &group = *binding.portGroup;
  auto model = group.module;
  auto top = hierarchy.top;
  auto instance = binding.instance;
  if (!model || !mlir::isa<FModuleOp>(model.getOperation()) ||
      instance->getParentOfType<FModuleOp>() != top ||
      instance.getModuleName() != model.getModuleName() ||
      binding.globalName != channel.name || group.name.empty() ||
      channel.name.empty() || channel.kind != ChannelKind::TargetClock ||
      group.direction != Direction::In || group.clockPort || channel.clock ||
      !channel.sources.empty())
    return reject("expected an unclocked input clock channel on the bound hub");
  if (group.ports.empty() || group.ports.size() != channel.sinks.size() ||
      group.ports.size() != channel.targetClocks.size() ||
      group.ports.size() != binding.instancePorts.size())
    return reject("clock ports, sinks, binding and metadata counts differ");

  // bindChannelToModels uses a sorted set to find the local group. Recover the
  // ordered association from actual top/instance connections, as SFC's
  // portsByInputChannel does, and verify the local annotation agrees.
  llvm::SmallVector<FAMEHubClockDomain> domains;
  std::set<unsigned> modelPorts, topPorts;
  std::set<std::string> fields;
  auto payloadField = [](llvm::StringRef name, llvm::StringRef channelName) {
    while (!name.empty() && !channelName.empty() &&
           name.front() == channelName.front()) {
      name = name.drop_front();
      channelName = channelName.drop_front();
    }
    return name.str();
  };
  for (unsigned i = 0; i < group.ports.size(); ++i) {
    unsigned modelPort = group.ports[i];
    const auto &sink = channel.sinks[i];
    if (modelPort >= model.getNumPorts() ||
        !modelPorts.insert(modelPort).second ||
        !llvm::is_contained(binding.instancePorts, modelPort) ||
        model.getPortDirection(modelPort) != Direction::In ||
        !mlir::isa<ClockType>(model.getPorts()[modelPort].type))
      return reject("model clock port is repeated, unbound or not an input Clock");
    if (sink.module != top || !sink.port ||
        *sink.port >= top.getNumPorts() ||
        (sink.fieldID && *sink.fieldID != 0) ||
        !topPorts.insert(*sink.port).second ||
        top.getPortDirection(*sink.port) != Direction::In ||
        !mlir::isa<ClockType>(top.getPorts()[*sink.port].type))
      return reject("clock sink is repeated or not a top input Clock");
    unsigned matches = 0;
    for (const auto &connection : hierarchy.connections) {
      if (connection.instance != binding.instance)
        continue;
      if (connection.instancePort == modelPort) {
        if (connection.topPort != *sink.port)
          return reject("model clock annotation order disagrees with sinks");
        ++matches;
      } else if (connection.topPort == *sink.port) {
        return reject("clock sink connects to a different model clock");
      }
    }
    if (matches != 1)
      return reject("clock domain needs one direct top-to-model connection");
    const auto &info = channel.targetClocks[i];
    if (info.name.empty() || !info.multiplier || !info.divisor || !info.mfmr)
      return reject("clock metadata has an empty name or zero ratio/MFMR");
    auto modelName = model.getPortName(modelPort).str();
    auto topName = top.getPortName(*sink.port).str();
    std::string field;
    if (group.ports.size() > 1) {
      field = payloadField(modelName, group.name);
      if (field.empty() || !fields.insert(field).second ||
          field != payloadField(topName, channel.name))
        return reject("model and top clock payload fields disagree");
    }
    auto resolvedInfo = info;
    // Scala's clockMFMRMap is clockInfo.zip(clockMFMRs).toMap: repeated
    // RationalClock records select the last MFMR for that exact clock key.
    for (const auto &other : channel.targetClocks)
      if (other.name == info.name && other.multiplier == info.multiplier &&
          other.divisor == info.divisor)
        resolvedInfo.mfmr = other.mfmr;
    domains.push_back({modelPort, *sink.port, std::move(modelName),
                       std::move(topName), std::move(field), resolvedInfo});
  }
  return domains;
}

// SFC's getHostDecoupledChannelType preserves the original endpoint type for a
// single leaf, or groups multiple leaves under their names after removing the
// common prefix with the global channel name. Construct the corresponding
// FIRRTL types here so a later rewrite can insert them directly on FModuleOp.
static std::optional<BundleType>
channelPortType(const goldengate::GGChannelConnection &channel,
                bool source, mlir::MLIRContext *context, std::string &error) {
  auto &endpoints = source ? channel.sources : channel.sinks;
  if (endpoints.empty()) {
    error = "transformed channel has no endpoints: " + channel.name;
    return std::nullopt;
  }
  FIRRTLBaseType payload;
  if (endpoints.size() == 1) {
    auto module = endpoints.front().module;
    payload = mlir::dyn_cast<FIRRTLBaseType>(
        module.getPorts()[*endpoints.front().port].type);
    if (!payload) {
      error = "channel endpoint has no FIRRTL hardware type: " + channel.name;
      return std::nullopt;
    }
  } else {
    llvm::SmallVector<BundleType::BundleElement> fields;
    std::set<std::string> fieldNames;
    for (const auto &endpoint : endpoints) {
      auto module = endpoint.module;
      auto name = module.getPortName(*endpoint.port);
      auto prefix = llvm::StringRef(channel.name);
      while (!name.empty() && !prefix.empty() && name.front() == prefix.front()) {
        name = name.drop_front();
        prefix = prefix.drop_front();
      }
      if (name.empty() || !fieldNames.insert(name.str()).second) {
        error = "channel payload has an empty or duplicate field: " +
                channel.name;
        return std::nullopt;
      }
      auto fieldType = mlir::dyn_cast<FIRRTLBaseType>(
          module.getPorts()[*endpoint.port].type);
      if (!fieldType) {
        error = "channel endpoint has no FIRRTL hardware type: " + channel.name;
        return std::nullopt;
      }
      fields.emplace_back(mlir::StringAttr::get(context, name), false,
                          fieldType);
    }
    payload = BundleType::get(context, fields);
  }
  auto bit = UIntType::get(context, 1, false);
  return BundleType::get(context,
                         {{mlir::StringAttr::get(context, "ready"), true, bit},
                          {mlir::StringAttr::get(context, "valid"), false, bit},
                          {mlir::StringAttr::get(context, "bits"), false, payload}});
}

std::optional<goldengate::FAMEPortPlan>
goldengate::analyzeFAMEPorts(
    const TopHierarchy &hierarchy, llvm::ArrayRef<ModelChannelBinding> bindings,
    llvm::ArrayRef<GGChannelConnection> channels,
    llvm::ArrayRef<FModuleLike> transformedModules, std::string &error) {
  FAMEPortPlan plan;
  if (transformedModules.empty())
    return plan;

  std::set<std::string> newNames;
  std::set<unsigned> stale;
  bool hasHostClock = false;
  auto top = hierarchy.top;
  for (unsigned i = 0, n = top.getPorts().size(); i != n; ++i) {
    if (top.getPortName(i) == "hostClock") {
      hasHostClock = true;
      continue;
    }
    if (mlir::isa<ClockType>(top.getPorts()[i].type))
      stale.insert(i);
  }
  if (!hasHostClock) {
    error = "FAME top has no hostClock port";
    return std::nullopt;
  }

  auto orderedSources = [&](const GGChannelConnection &channel,
                            const ModelChannelBinding &binding) {
    llvm::SmallVector<unsigned> ports;
    for (const auto &endpoint : channel.sources)
      for (const auto &connection : hierarchy.connections)
        if (endpoint.port && endpoint.module == top &&
            connection.topPort == *endpoint.port &&
            connection.instance == binding.instance)
          ports.push_back(connection.instancePort);
    return ports;
  };

  for (const auto &binding : bindings) {
    auto model = binding.portGroup->module;
    if (!llvm::is_contained(transformedModules, model))
      continue;
    auto instance = binding.instance;
    bool source = binding.portGroup->direction == Direction::Out;
    std::string portName = instance.getName().str() + "_" +
                           binding.portGroup->name +
                           (source ? "_source" : "_sink");
    auto channel = llvm::find_if(channels, [&](const GGChannelConnection &c) {
      return c.name == binding.globalName;
    });
    if (channel == channels.end()) {
      error = "no global channel for model binding " + binding.globalName;
      return std::nullopt;
    }
    auto type = channelPortType(*channel, source, top.getContext(), error);
    if (!type)
      return std::nullopt;
    if (newNames.insert(portName).second) {
      auto &ports = source ? plan.sources : plan.sinks;
      ports.push_back({&binding, std::move(portName), *type});
    } else {
      // Multiple global branches may describe the same local output. SFC
      // creates one host source port, while preserving every branch annotation.
      auto previous = llvm::find_if(plan.sources, [&](const auto &port) {
        return port.portName == portName;
      });
      auto previousChannel = previous == plan.sources.end() ? channels.end()
          : llvm::find_if(channels, [&](const auto &entry) {
              return entry.name == previous->binding->globalName;
            });
      if (!source || previous == plan.sources.end() ||
          previous->binding->instance != binding.instance ||
          previous->binding->portGroup != binding.portGroup ||
          previousChannel == channels.end() || previousChannel->kind != channel->kind ||
          previous->type != *type ||
          orderedSources(*previousChannel, *previous->binding) !=
              orderedSources(*channel, binding)) {
        error = "two transformed channels claim top port " + portName;
        return std::nullopt;
      }
    }

    for (unsigned modelPort : binding.instancePorts) {
      bool found = false;
      for (const auto &connection : hierarchy.connections) {
        if (connection.instance == instance &&
            connection.instancePort == modelPort) {
          stale.insert(connection.topPort);
          found = true;
        }
      }
      if (!found) {
        error = "transformed model port has no top connection";
        return std::nullopt;
      }
    }
  }
  plan.staleTopPorts.append(stale.begin(), stale.end());
  return plan;
}
