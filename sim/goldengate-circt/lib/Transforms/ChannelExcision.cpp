// See LICENSE for license details.
#include "goldengate/ChannelExcision.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct BridgeChannel {
  DictionaryAttr annotation;
  std::string name;
  std::vector<unsigned> ports;
  std::vector<std::vector<std::string>> duplicatePorts;
};

bool isConnect(Operation *op) {
  return isa<ConnectOp, StrictConnectOp>(op);
}

std::optional<std::pair<InstanceOp, unsigned>> instancePort(Value value) {
  auto result = dyn_cast<OpResult>(value);
  if (!result)
    return std::nullopt;
  auto instance = dyn_cast<InstanceOp>(result.getOwner());
  if (!instance)
    return std::nullopt;
  return std::make_pair(instance, result.getResultNumber());
}

Attribute renameTargets(Attribute attr,
                        const std::map<std::string, std::string> &renames) {
  if (auto text = dyn_cast<StringAttr>(attr)) {
    auto found = renames.find(text.getValue().str());
    return found == renames.end() ? attr
                                  : StringAttr::get(attr.getContext(), found->second);
  }
  if (auto array = dyn_cast<ArrayAttr>(attr)) {
    SmallVector<Attribute> members;
    for (Attribute member : array)
      members.push_back(renameTargets(member, renames));
    return ArrayAttr::get(attr.getContext(), members);
  }
  if (auto dictionary = dyn_cast<DictionaryAttr>(attr)) {
    NamedAttrList members;
    for (NamedAttribute member : dictionary)
      members.set(member.getName(), renameTargets(member.getValue(), renames));
    return DictionaryAttr::get(attr.getContext(), members);
  }
  return attr;
}
} // namespace

LogicalResult goldengate::exciseChannels(CircuitOp circuit,
                                         std::string &error) {
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!top || !raw) {
    error = "ChannelExcision needs a top module and retained annotations";
    return failure();
  }
  auto *context = circuit.getContext();
  std::string topPrefix = "~" + circuit.getName().str() + "|" +
                          top.getName().str() + ">";
  std::map<std::string, unsigned> topPorts;
  std::set<std::string> usedNames;
  for (unsigned i = 0; i < top.getNumPorts(); ++i) {
    std::string name = top.getPortName(i).str();
    topPorts.emplace(name, i);
    usedNames.insert(name);
  }
  for (auto instance : top.getOps<InstanceOp>())
    usedNames.insert(instance.getName().str());

  std::vector<BridgeChannel> channels;
  std::set<unsigned> bridgePorts;
  std::map<unsigned, std::pair<unsigned, unsigned>> bridgePortBinding;
  std::set<std::pair<std::string, std::string>> modelPipes;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") !=
                     AnnotationClasses::PipeChannel)
      continue;
    auto sources = annotation.getMember<ArrayAttr>("sources");
    auto sinks = annotation.getMember<ArrayAttr>("sinks");
    if (sources && sinks) {
      for (unsigned i = 0; i < std::min(sources.size(), sinks.size()); ++i) {
        auto source = dyn_cast<StringAttr>(sources[i]);
        auto sink = dyn_cast<StringAttr>(sinks[i]);
        if (!source || !sink) {
          error = "inter-model pipe has a non-reference endpoint";
          return failure();
        }
        modelPipes.emplace(sink.getValue().str(), source.getValue().str());
      }
      continue;
    }
    if (sources || !sinks)
      continue;
    auto name = annotation.getMember<StringAttr>("globalName");
    if (!name || sinks.empty()) {
      error = "bridge-sourced pipe channel must have a name and sinks";
      return failure();
    }
    BridgeChannel channel{cast<DictionaryAttr>(attr), name.getValue().str(),
                          {}, {}};
    for (Attribute sinkAttr : sinks) {
      auto sink = dyn_cast<StringAttr>(sinkAttr);
      if (!sink || !sink.getValue().starts_with(topPrefix)) {
        error = "bridge-sourced pipe sink is not a wrapper input";
        return failure();
      }
      std::string portName = sink.getValue().drop_front(topPrefix.size()).str();
      auto found = topPorts.find(portName);
      if (found == topPorts.end() ||
          top.getPortDirection(found->second) != Direction::In ||
          !bridgePorts.insert(found->second).second) {
        error = "bridge-sourced pipe sink has no unique input port: " +
                portName;
        return failure();
      }
      bridgePortBinding.emplace(found->second,
                                std::make_pair(channels.size(), channel.ports.size()));
      channel.ports.push_back(found->second);
      channel.duplicatePorts.push_back({portName});
    }
    channels.push_back(std::move(channel));
  }

  // The Scala pass walks connections in statement order. Collect operations
  // before inserting ports so that changing a module signature cannot affect
  // the walk or the identity of an existing block argument.
  std::vector<Operation *> connections;
  top.walk([&](Operation *op) {
    if (isConnect(op))
      connections.push_back(op);
  });

  std::map<unsigned, unsigned> bridgeUseCount;
  llvm::DenseMap<Value, unsigned> modelSourcePorts;
  std::map<std::string, std::string> renames;
  auto newPort = [&](const std::string &name, Type type, Direction direction) {
    unsigned index = top.getNumPorts();
    top.insertPorts({{index, PortInfo(StringAttr::get(context, name), type,
                                      direction)}});
    usedNames.insert(name);
    return top.getBodyBlock()->getArgument(index);
  };
  auto uniqueName = [&](std::string base) {
    std::string name = base;
    for (unsigned suffix = 0; usedNames.count(name); ++suffix)
      name = base + "_" + std::to_string(suffix);
    return name;
  };
  for (Operation *op : connections) {
    Value dest = op->getOperand(0), source = op->getOperand(1);
    auto lhs = instancePort(dest), rhs = instancePort(source);
    if (lhs && rhs) {
      std::string lhsTarget = topPrefix + lhs->first.getName().str() + "." +
                              lhs->first.getPortNameStr(lhs->second).str();
      std::string rhsTarget = topPrefix + rhs->first.getName().str() + "." +
                              rhs->first.getPortNameStr(rhs->second).str();
      if (!modelPipes.count({lhsTarget, rhsTarget}) &&
          !isa<ClockType>(dest.getType()))
        continue;
      unsigned sourceIndex;
      if (auto found = modelSourcePorts.find(source);
          found != modelSourcePorts.end()) {
        sourceIndex = found->second;
      } else {
        std::string sourceName = rhs->first.getName().str() + "_" +
                                 rhs->first.getPortNameStr(rhs->second).str() +
                                 "_source";
        if (usedNames.count(sourceName)) {
          error = "model source wrapper port already exists: " + sourceName;
          return failure();
        }
        sourceIndex = top.getNumPorts();
        newPort(sourceName, dest.getType(), Direction::Out);
        modelSourcePorts.insert({source, sourceIndex});
        renames.emplace(rhsTarget, topPrefix + sourceName);
      }
      std::string sinkName = uniqueName(lhs->first.getName().str() + "_" +
                                        lhs->first.getPortNameStr(lhs->second).str() +
                                        "_sink");
      Value sink = newPort(sinkName, source.getType(), Direction::In);
      renames.emplace(lhsTarget, topPrefix + sinkName);
      op->setOperand(1, sink);
      OpBuilder builder(op);
      builder.setInsertionPointAfter(op);
      builder.create<ConnectOp>(op->getLoc(),
                                top.getBodyBlock()->getArgument(sourceIndex),
                                source);
      continue;
    }

    auto argument = dyn_cast<BlockArgument>(source);
    if (!argument || argument.getOwner() != top.getBodyBlock() ||
        !bridgePorts.count(argument.getArgNumber()))
      continue;
    bool destinationIsTopOutput = false;
    if (auto destArgument = dyn_cast<BlockArgument>(dest))
      destinationIsTopOutput = destArgument.getOwner() == top.getBodyBlock() &&
          top.getPortDirection(destArgument.getArgNumber()) == Direction::Out;
    if (!lhs && !destinationIsTopOutput) {
      error = "bridge-sourced pipe has a non-channel destination";
      return failure();
    }
    unsigned port = argument.getArgNumber();
    unsigned use = bridgeUseCount[port]++;
    if (!use)
      continue;
    std::string base = top.getPortName(port).str();
    std::string name = uniqueName(base + "_" + std::to_string(use));
    Value input = newPort(name, source.getType(), Direction::In);
    op->setOperand(1, input);
    auto [channelIndex, sinkIndex] = bridgePortBinding.at(port);
    channels[channelIndex].duplicatePorts[sinkIndex].push_back(name);
  }
  for (const auto &channel : channels) {
    unsigned fanout = channel.duplicatePorts.front().size();
    for (unsigned i = 0; i < channel.ports.size(); ++i) {
      if (!bridgeUseCount[channel.ports[i]]) {
        error = "bridge-sourced pipe has no top-level connection: " +
                channel.name;
        return failure();
      }
      if (channel.duplicatePorts[i].size() != fanout) {
        error = "bridge-sourced pipe sinks have different fanout counts: " +
                channel.name;
        return failure();
      }
    }
  }

  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  if (!renames.empty())
    for (Attribute &attr : annotations)
      attr = renameTargets(attr, renames);
  auto key = [&](StringRef name) { return StringAttr::get(context, name); };
  for (const auto &channel : channels) {
    SmallVector<Attribute> names{key(channel.name)};
    for (unsigned i = 1; i < channel.duplicatePorts.front().size(); ++i)
      names.push_back(key(channel.name + "_" + std::to_string(i)));
    annotations.push_back(DictionaryAttr::get(
        context, {{key("class"), key(AnnotationClasses::ChannelFanout)},
                  {key("channelNames"), ArrayAttr::get(context, names)}}));
    for (unsigned i = 1; i < channel.duplicatePorts.front().size(); ++i) {
      NamedAttrList copy(cast<DictionaryAttr>(
          renameTargets(channel.annotation, renames)));
      copy.set("globalName", names[i]);
      SmallVector<Attribute> sinks;
      for (const auto &ports : channel.duplicatePorts)
        sinks.push_back(key(topPrefix + ports[i]));
      copy.set("sinks", ArrayAttr::get(context, sinks));
      annotations.push_back(DictionaryAttr::get(context, copy));
    }
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(context, annotations));
  return success();
}
