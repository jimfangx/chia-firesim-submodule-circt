// See LICENSE for license details.
// Oracle: ChannelClockInfoAnalysis, UpdateBridgeClockInfo and SimulationMapping.
// Clock records come exclusively from retained analysis, never host clocks.
#include "goldengate/BridgeClockInfo.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include <functional>
#include <algorithm>
#include <cstdint>

using namespace mlir;
using namespace circt::firrtl;
namespace {
bool clockMap(CircuitOp circuit, DictionaryAttr &map, unsigned &index,
              std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "bridge clock analysis needs retained annotations"; return false; }
  for (auto [i, attr] : llvm::enumerate(raw)) {
    auto row = dyn_cast<DictionaryAttr>(attr);
    auto cls = row ? row.getAs<StringAttr>("class") : StringAttr();
    if (!cls) { error = "malformed bridge clock annotation"; return false; }
    if (cls != goldengate::AnnotationClasses::ChannelClockInfo) continue;
    if (map) { error = "ambiguous channel clock analysis"; return false; }
    map = row; index = i;
    if (!map.getAs<DictionaryAttr>("infoMap")) {
      error = "channel clock analysis lacks infoMap"; return false;
    }
  }
  return true;
}
bool validClock(DictionaryAttr clock) {
  auto name = clock ? clock.getAs<StringAttr>("name") : StringAttr();
  auto m = clock ? clock.getAs<IntegerAttr>("multiplier") : IntegerAttr();
  auto d = clock ? clock.getAs<IntegerAttr>("divisor") : IntegerAttr();
  return name && !name.getValue().empty() && !name.getValue().contains('\0') &&
      m && m.getInt() > 0 && m.getInt() <= UINT32_MAX &&
      d && d.getInt() > 0 && d.getInt() <= UINT32_MAX;
}
} // namespace

std::optional<goldengate::GGTarget> goldengate::resolveBridgeClockInfo(
    CircuitOp circuit, DictionaryAttr bridge, std::string &error) {
  auto reject = [&](StringRef why) -> std::optional<GGTarget> {
    error = why.str(); return std::nullopt;
  };
  if (!bridge || bridge.getAs<StringAttr>("class") != AnnotationClasses::BridgeIO)
    return reject("expected retained BridgeIO clock identity");
  DictionaryAttr map; unsigned index = 0;
  if (!clockMap(circuit, map, index, error)) return std::nullopt;
  auto info = map ? map.getAs<DictionaryAttr>("infoMap") : DictionaryAttr();
  auto clock = bridge.getAs<DictionaryAttr>("clockInfo");
  auto channels = bridge.getAs<DictionaryAttr>("channelMapping");
  if (!info || !validClock(clock) || !channels || channels.empty())
    return reject("bridge needs resolved rational clock and channel analysis");
  llvm::StringSet<> names;
  for (auto member : channels) {
    auto channel = dyn_cast<StringAttr>(member.getValue());
    if (!channel || channel.getValue().empty() ||
        !names.insert(channel.getValue()).second ||
        info.getAs<DictionaryAttr>(channel.getValue()) != clock)
      return reject("bridge clock differs from its unique analyzed channels");
  }
  auto target = bridge.getAs<StringAttr>("target");
  auto resolved = target ? resolveAnnotationTarget(circuit, target.getValue(), error) : std::nullopt;
  if (!resolved || !resolved->port) return reject("bridge clock target must resolve to a FIRRTL port");
  return resolved;
}

LogicalResult goldengate::importBridgeClockInfo(CircuitOp destination,
    CircuitOp source, StringRef widgetClass, std::string &error) {
  auto reject = [&](StringRef why) { error = why.str(); return failure(); };
  if (source == destination || widgetClass.empty() || source.getContext() != destination.getContext())
    return reject("bridge clock import requires distinct circuits in one context");
  DictionaryAttr sourceMap, destinationMap; unsigned sourceIndex = 0, destinationIndex = 0;
  if (!clockMap(source, sourceMap, sourceIndex, error) ||
      !clockMap(destination, destinationMap, destinationIndex, error)) return failure();
  DictionaryAttr bridge;
  for (auto attr : source->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    auto row = cast<DictionaryAttr>(attr);
    if (row.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
        row.getAs<StringAttr>("widgetClass") == widgetClass) {
      if (bridge) return reject("ambiguous imported bridge constructor");
      bridge = row;
    }
  }
  auto resolved = resolveBridgeClockInfo(source, bridge, error);
  if (!resolved) return failure();
  Builder b(destination.getContext());
  auto spelling = bridge.getAs<StringAttr>("target").getValue();
  auto suffix = spelling.drop_front(1 + source.getName().size());
  auto target = b.getStringAttr("~" + destination.getName().str() + suffix.str());
  auto imported = resolveAnnotationTarget(destination, target.getValue(), error);
  auto module = imported ? dyn_cast<FModuleOp>(imported->module.getOperation()) : FModuleOp();
  auto key = bridge.getAs<DictionaryAttr>("widgetConstructorKey");
  auto sourceModule = dyn_cast<FModuleOp>(resolved->module.getOperation());
  if (!imported || !module || !sourceModule || !key || !imported->port ||
      sourceModule->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") != key ||
      module.getArgument(*imported->port).getType() != module.getPortType(*imported->port) ||
      sourceModule.getArgument(*resolved->port).getType() != sourceModule.getPortType(*resolved->port) ||
      imported->module.getModuleName() != resolved->module.getModuleName() ||
      imported->port != resolved->port || imported->fieldID != resolved->fieldID ||
      module.getPortType(*imported->port) != resolved->module.getPortType(*resolved->port) ||
      module.getPortDirection(*imported->port) != resolved->module.getPortDirection(*resolved->port) ||
      module->getAttrOfType<DictionaryAttr>("goldengate.bridgeConstructor") != key)
    return reject("imported bridge constructor or FIRRTL port identity differs");
  InstanceGraph graph(destination);
  auto *top = graph.lookup(b.getStringAttr(destination.getName()));
  auto *anchor = graph.lookup(module.getNameAttr());
  llvm::DenseMap<circt::igraph::InstanceGraphNode *, unsigned> counts;
  llvm::DenseSet<circt::igraph::InstanceGraphNode *> active;
  bool recursive = false;
  std::function<unsigned(circt::igraph::InstanceGraphNode *)> paths = [&](auto *node) {
    if (active.count(node)) { recursive = true; return 0U; }
    if (counts.count(node)) return counts[node];
    active.insert(node); unsigned n = node == anchor;
    for (auto *edge : *node) n = std::min(2U, n + paths(edge->getTarget()));
    active.erase(node); counts[node] = n; return n;
  };
  if (!top || !top->noUses() || paths(top) != 1 || recursive)
    return reject("imported bridge needs one live instance path");
  auto raw = destination->getAttrOfType<ArrayAttr>("rawAnnotations");
  for (auto attr : raw) {
    auto row = cast<DictionaryAttr>(attr);
    if (row.getAs<StringAttr>("class") == AnnotationClasses::BridgeIO &&
        (row.getAs<StringAttr>("widgetClass") == widgetClass || row.getAs<StringAttr>("target") == target))
      return reject("imported bridge clock identity already exists");
  }
  NamedAttrList entries(destinationMap ? destinationMap.getAs<DictionaryAttr>("infoMap") : b.getDictionaryAttr({}));
  auto sourceEntries = sourceMap.getAs<DictionaryAttr>("infoMap");
  for (auto member : bridge.getAs<DictionaryAttr>("channelMapping")) {
    auto name = cast<StringAttr>(member.getValue()).getValue();
    auto clock = sourceEntries.get(name);
    if (auto prior = entries.get(name); prior && prior != clock)
      return reject("imported channel conflicts with destination clock analysis");
    entries.set(name, clock);
  }
  NamedAttrList map(destinationMap ? destinationMap : b.getDictionaryAttr({
      b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelClockInfo))}));
  map.set("infoMap", entries.getDictionary(destination.getContext()));
  NamedAttrList importedBridge(bridge); importedBridge.set("target", target);
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  if (destinationMap) annotations[destinationIndex] = map.getDictionary(destination.getContext());
  else annotations.push_back(map.getDictionary(destination.getContext()));
  annotations.push_back(importedBridge.getDictionary(destination.getContext()));
  destination->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
