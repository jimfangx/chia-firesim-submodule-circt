// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "mlir/IR/Builders.h"
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::rewriteFAMEHubClockChannel(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    llvm::ArrayRef<FAMEHubClockDomain> domains, bool transferDebug,
    std::string &error) {
  auto reject = [&](llvm::StringRef reason) {
    error = reason.str();
    return failure();
  };
  auto top = hierarchy.top;
  if (!top)
    return reject("hub clock rewrite requires a top module");
  auto circuit = top->getParentOfType<CircuitOp>();
  if (!circuit)
    return reject("hub clock rewrite requires a FIRRTL circuit");
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw || !channel.binding || !channel.binding->portGroup || domains.empty())
    return reject("hub clock rewrite requires retained annotations and an ordered domain plan");
  auto group = *channel.binding->portGroup;
  auto model = dyn_cast<FModuleOp>(group.module.getOperation());
  auto channelType = channel.type;
  auto bitsIndex = channelType.getElementIndex("bits");
  if (!model || group.direction != Direction::In || group.clockPort ||
      group.ports.size() != domains.size() || !bitsIndex)
    return reject("hub clock rewrite requires one input group with matching domain arity");
  auto payload = channel.type.getElements()[*bitsIndex].type;
  auto bundle = dyn_cast<BundleType>(payload);
  if (domains.size() == 1 ? !isa<ClockType>(payload)
                          : (!bundle || bundle.getElements().size() != domains.size()))
    return reject("hub clock payload does not match ordered domains");

  struct Rename { std::string replacement; unsigned count = 0; };
  std::map<std::string, Rename> topRenames, modelRenames;
  SmallVector<std::string> orderedTopTargets, orderedModelTargets;
  std::map<std::string, std::string> debugRenames;
  std::string prefix = "~" + circuit.getName().str() + "|";
  auto modelChannel = group.name + "_sink";
  std::set<std::string> fields;
  for (unsigned i = 0; i < domains.size(); ++i) {
    const auto &d = domains[i];
    if (d.modelPort >= model.getNumPorts() || d.topPort >= top.getNumPorts() ||
        group.ports[i] != d.modelPort ||
        model.getPortName(d.modelPort) != d.modelClockName ||
        top.getPortName(d.topPort) != d.topClockName ||
        model.getPortDirection(d.modelPort) != Direction::In ||
        top.getPortDirection(d.topPort) != Direction::In ||
        !isa<ClockType>(model.getPorts()[d.modelPort].type) ||
        !isa<ClockType>(top.getPorts()[d.topPort].type))
      return reject("hub clock rewrite domain identity is stale");
    if (domains.size() == 1 ? !d.payloadField.empty()
                            : (d.payloadField.empty() || !fields.insert(d.payloadField).second ||
                               bundle.getElements()[i].name.getValue() != d.payloadField ||
                               bundle.getElements()[i].isFlip ||
                               !isa<ClockType>(bundle.getElements()[i].type)))
      return reject("hub clock rewrite payload field order differs from domain plan");
    auto suffix = ".bits" + (d.payloadField.empty() ? std::string() : "." + d.payloadField);
    auto add = [&](FModuleOp module, const std::string &oldName,
                   const std::string &newName, auto &renames) {
      auto local = module.getName().str() + ">";
      auto oldTarget = prefix + local + oldName;
      auto newTarget = prefix + local + newName + suffix;
      if (!renames.emplace(oldTarget, Rename{newTarget}).second) return false;
      debugRenames.emplace(oldTarget, newTarget);
      auto legacy = circuit.getName().str() + "." + module.getName().str() + ".";
      debugRenames.emplace(legacy + oldName, legacy + newName + suffix);
      return true;
    };
    if (!add(top, d.topClockName, channel.portName, topRenames) ||
        !add(model, d.modelClockName, modelChannel, modelRenames))
      return reject("hub clock rewrite domain targets are duplicated");
    orderedTopTargets.push_back(prefix + top.getName().str() + ">" + d.topClockName);
    orderedModelTargets.push_back(prefix + model.getName().str() + ">" + d.modelClockName);
  }

  // Build the complete retained-annotation update before erasing any ports.
  // Preserve annotation and target-list order, including unrelated entries.
  SmallVector<Attribute> updated;
  auto rewriteTargets = [&](Annotation &anno, llvm::StringRef member, auto &renames,
                            const SmallVector<std::string> &expected) {
    auto entries = anno.getMember<ArrayAttr>(member);
    if (!entries) return true;
    SmallVector<Attribute> replaced;
    SmallVector<std::string> matched;
    for (auto entry : entries) {
      auto target = dyn_cast<StringAttr>(entry);
      auto found = target ? renames.find(target.getValue().str()) : renames.end();
      if (found != renames.end()) {
        matched.push_back(found->first);
        ++found->second.count;
        replaced.push_back(StringAttr::get(circuit.getContext(), found->second.replacement));
      } else replaced.push_back(entry);
    }
    if (!matched.empty() && matched != expected) return false;
    anno.setMember(member, ArrayAttr::get(circuit.getContext(), replaced));
    return true;
  };
  // SFC's hostDecouplingRenames also applies to ChannelConnection.clock and
  // ChannelPorts.clockPort. Transfer exact input identities to the same Clock
  // payload leaves; surviving output aliases and unrelated domains stay put.
  // Associated references may be shared by many channels and do not count as
  // occurrences of the clock channel's own endpoint lists.
  auto rewriteAssociatedClock = [&](Annotation &anno, llvm::StringRef member) {
    auto target = anno.getMember<StringAttr>(member);
    if (!target) return;
    for (const auto *renames : {&topRenames, &modelRenames}) {
      auto found = renames->find(target.getValue().str());
      if (found == renames->end()) continue;
      anno.setMember(member, StringAttr::get(circuit.getContext(), found->second.replacement));
      return;
    }
  };
  for (auto attr : raw) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::ChannelConnection) &&
        !rewriteTargets(anno, "sinks", topRenames, orderedTopTargets))
      return reject("retained hub clock sinks disagree with domain order");
    if (anno.isClass(AnnotationClasses::ChannelPorts) &&
        !rewriteTargets(anno, "ports", modelRenames, orderedModelTargets))
      return reject("retained hub clock ports disagree with domain order");
    if (anno.isClass(AnnotationClasses::ChannelConnection))
      rewriteAssociatedClock(anno, "clock");
    if (anno.isClass(AnnotationClasses::ChannelPorts))
      rewriteAssociatedClock(anno, "clockPort");
    if (transferDebug && anno.isClass(AnnotationClasses::InternalFpgaDebug)) {
      auto target = anno.getMember<StringAttr>("target");
      if (!target) return reject("FPGA debug annotation lacks a string target");
      auto found = debugRenames.find(target.getValue().str());
      if (found != debugRenames.end())
        anno.setMember("target", StringAttr::get(circuit.getContext(), found->second));
    }
    updated.push_back(anno.getAttr());
  }
  for (const auto &renames : {&topRenames, &modelRenames})
    for (const auto &entry : *renames)
      if (entry.second.count != 1)
        return reject("hub clock annotation targets must occur exactly once");
  if (failed(rewriteFAMEInputChannel(hierarchy, channel, error))) return failure();
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), updated));
  return success();
}
