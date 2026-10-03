// See LICENSE for license details.
#include "goldengate/FAMEPortAnalysis.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include <set>

using namespace circt::firrtl;

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

  for (const auto &binding : bindings) {
    auto model = binding.portGroup->module;
    if (!llvm::is_contained(transformedModules, model))
      continue;
    auto instance = binding.instance;
    bool source = binding.portGroup->direction == Direction::Out;
    std::string portName = instance.getName().str() + "_" +
                           binding.portGroup->name +
                           (source ? "_source" : "_sink");
    if (!newNames.insert(portName).second) {
      error = "two transformed channels claim top port " + portName;
      return std::nullopt;
    }
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
    auto &ports = source ? plan.sources : plan.sinks;
    ports.push_back({&binding, std::move(portName), *type});

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
