// See LICENSE for license details.
#include "goldengate/DeduplicateModels.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Pass/PassManager.h"
#include <functional>
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;
namespace {
using Modules = std::map<std::string, FModuleLike>;
Modules modules(CircuitOp circuit) {
  Modules result;
  for (Operation &op : *circuit.getBodyBlock())
    if (auto module = dyn_cast<FModuleLike>(&op))
      result.emplace(module.getModuleName().str(), module);
  return result;
}
LogicalResult apply(CircuitOp original, CircuitOp staged, unsigned &removed,
                    std::string &error) {
  auto reject = [&](const std::string &message) { error = message; return failure(); };
  auto raw = staged->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return reject("dedup needs retained annotations");
  auto before = modules(original);
  auto initial = modules(staged);
  // CIRCT sees module annotations, whereas this compiler retains serialized
  // targets. Import only the recognized dedup policy annotations for the pass.
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    bool group = annotation.isClass("firrtl.transforms.DedupGroupAnnotation");
    if (!group && !annotation.isClass("firrtl.transforms.NoDedupAnnotation")) continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) return reject("dedup policy has no target");
    auto spelling = target.getValue();
    StringRef name = spelling.starts_with("~") ? spelling.split('|').second
                                               : spelling.split('.').second;
    if (name.empty() || name.contains('/') || name.contains('>'))
      return reject("dedup policy has an unsupported module target");
    // Earlier extraction may have removed an annotated bridge definition.
    if (!initial.count(name.str())) continue;
    auto module = initial.at(name.str());
    SmallVector<Attribute> annotations;
    if (auto existing = module->getAttrOfType<ArrayAttr>("annotations"))
      annotations.append(existing.begin(), existing.end());
    NamedAttrList members(cast<DictionaryAttr>(attr)); members.erase("target");
    annotations.push_back(members.getDictionary(staged.getContext()));
    module->setAttr("annotations", ArrayAttr::get(staged.getContext(), annotations));
  }
  PassManager manager(staged.getContext(), CircuitOp::getOperationName());
  manager.addPass(createDedupPass());
  if (failed(manager.run(staged))) return reject("native FIRRTL dedup failed");
  auto after = modules(staged);
  std::map<std::string, std::string> mapping;
  std::map<std::string, std::map<std::string, std::string>> portNames;
  std::map<std::string, std::map<std::string, std::string>> instanceNames;
  std::set<std::pair<std::string, std::string>> visited;
  std::function<LogicalResult(std::string, std::string)> bind =
      [&](std::string oldName, std::string newName) -> LogicalResult {
    if (!before.count(oldName) || !after.count(newName))
      return reject("dedup witness has a missing module");
    if (mapping.count(oldName) && mapping.at(oldName) != newName)
      return reject("dedup witnesses disagree for " + oldName);
    mapping[oldName] = newName;
    if (!visited.emplace(oldName, newName).second) return success();
    auto oldModule = before.at(oldName), newModule = after.at(newName);
    if (oldModule.getNumPorts() != newModule.getNumPorts())
      return reject("dedup changed port count for " + oldName);
    for (unsigned i = 0; i < oldModule.getNumPorts(); ++i)
      if (oldModule.getPortType(i) != newModule.getPortType(i) ||
          oldModule.getPortDirection(i) != newModule.getPortDirection(i))
        return reject("dedup changed port type/direction for " + oldName);
    for (unsigned i = 0; i < oldModule.getNumPorts(); ++i)
      portNames[oldName][oldModule.getPortName(i).str()] = newModule.getPortName(i).str();
    SmallVector<InstanceOp> oldChildren, newChildren;
    oldModule->walk([&](InstanceOp instance) { oldChildren.push_back(instance); });
    newModule->walk([&](InstanceOp instance) { newChildren.push_back(instance); });
    if (oldChildren.size() != newChildren.size())
      return reject("dedup changed instance count for " + oldName);
    for (unsigned i = 0; i < oldChildren.size(); ++i) {
      instanceNames[oldName][oldChildren[i].getName().str()] = newChildren[i].getName().str();
      if (failed(bind(oldChildren[i].getModuleName().str(),
                      newChildren[i].getModuleName().str()))) return failure();
    }
    return success();
  };
  if (failed(bind(original.getName().str(), staged.getName().str()))) return failure();
  for (auto &[name, module] : before)
    if (!mapping.count(name)) {
      if (!after.count(name)) return reject("dedup removed an unreachable module without a witness: " + name);
      if (failed(bind(name, name))) return failure();
    }

  // Native dedup ignores names. Recover their correspondence only after
  // proving complete operation/operand equivalence with explicit positional
  // port and child-module bindings. This prevents a same-named canonical value
  // from silently referring to a different expression.
  std::map<std::string, bool> namedEquivalent;
  std::map<std::string, std::map<std::string, std::string>> references;
  auto provesInternalNames = [&](const std::string &oldName, const std::string &newName) {
    auto found = namedEquivalent.find(oldName);
    if (found != namedEquivalent.end()) return found->second;
    auto stripPolicy = [&](Operation *module) {
      if (auto annotations = module->getAttrOfType<ArrayAttr>("annotations")) {
        SmallVector<Attribute> retained;
        for (Attribute attr : annotations) {
          Annotation annotation(attr);
          if (!annotation.isClass("firrtl.transforms.DedupGroupAnnotation") &&
              !annotation.isClass("firrtl.transforms.NoDedupAnnotation")) retained.push_back(attr);
        }
        module->setAttr("annotations", ArrayAttr::get(staged.getContext(), retained));
      }
    };
    OwningOpRef<Operation *> normalized(before.at(oldName)->clone());
    OwningOpRef<Operation *> canonical(after.at(newName)->clone());
    (*normalized)->setAttr("sym_name", StringAttr::get(staged.getContext(), newName));
    if (auto names = (*canonical)->getAttr("portNames")) (*normalized)->setAttr("portNames", names);
    (*normalized)->setAttr(FModuleLike::getPortLocationsAttrName(), after.at(newName).getPortLocationsAttr());
    SmallVector<Operation *> oldOps, newOps;
    normalized->walk([&](Operation *op) { oldOps.push_back(op); });
    canonical->walk([&](Operation *op) { newOps.push_back(op); });
    if (oldOps.size() != newOps.size()) { namedEquivalent[oldName] = false; return false; }
    bool namesValid = true;
    for (unsigned i = 0; i < oldOps.size(); ++i) {
      auto old = oldOps[i]->getAttrOfType<StringAttr>("name");
      auto peer = newOps[i]->getAttrOfType<StringAttr>("name");
      if (bool(old) != bool(peer)) { namesValid = false; break; }
      if (old) {
        auto inserted = references[oldName].emplace(old.getValue().str(), peer.getValue().str());
        if (!inserted.second && inserted.first->second != peer.getValue()) { namesValid = false; break; }
        oldOps[i]->setAttr("name", StringAttr::get(staged.getContext(), ""));
        newOps[i]->setAttr("name", StringAttr::get(staged.getContext(), ""));
      }
      if (auto child = dyn_cast<InstanceOp>(oldOps[i])) {
        auto mapped = mapping.find(child.getModuleName().str());
        if (mapped == mapping.end()) { namesValid = false; break; }
        child->setAttr("moduleName", FlatSymbolRefAttr::get(staged.getContext(), mapped->second));
        auto peerChild = dyn_cast<InstanceOp>(newOps[i]);
        if (!peerChild) { namesValid = false; break; }
        child->setAttr("portNames", peerChild->getAttr("portNames"));
      }
    }
    stripPolicy(*normalized); stripPolicy(*canonical);
    bool equivalent = namesValid && OperationEquivalence::isEquivalentTo(*normalized, *canonical,
                                         OperationEquivalence::IgnoreLocations);
    namedEquivalent.emplace(oldName, equivalent);
    return equivalent;
  };
  auto isPort = [&](const std::string &name, StringRef reference) {
    for (auto port : before.at(name).getPorts())
      if (port.getName() == reference) return true;
    return false;
  };
  auto mappedReference = [&](const std::string &owner, StringRef reference)
      -> std::optional<std::string> {
    auto &names = references[owner];
    auto direct = names.find(reference.str());
    if (direct != names.end()) return direct->second;
    // Some known legacy annotations retain a pre-LowerTypes aggregate root.
    // Reconstruct its name only when ALL proved scalar descendants preserve
    // their complete leaf suffix and agree on one canonical aggregate prefix.
    std::optional<std::string> prefix;
    bool mismatch = false;
    auto inspect = [&](const auto &entries) {
      for (const auto &[old, peer] : entries) {
        if (!StringRef(old).starts_with(reference.str() + "_")) continue;
        auto suffix = StringRef(old).drop_front(reference.size());
        if (!StringRef(peer).ends_with(suffix)) { mismatch = true; continue; }
        auto candidate = StringRef(peer).drop_back(suffix.size()).str();
        if (prefix && *prefix != candidate) mismatch = true;
        prefix = candidate;
      }
    };
    inspect(names); inspect(portNames[owner]);
    return mismatch ? std::nullopt : prefix;
  };
  // Retarget only explicit target encodings. The compiler does not reinterpret
  // arbitrary annotation strings (Verilog payloads, type names, group labels).
  std::function<std::optional<std::string>(StringRef, bool)> renameTarget =
      [&](StringRef spelling, bool legacy) -> std::optional<std::string> {
    if (!spelling.starts_with("~")) {
      if (!legacy) return spelling.str();
      auto [circuitName, local] = spelling.split('.');
      auto [owner, rest] = local.split('.');
      auto found = mapping.find(owner.str());
      if (found == mapping.end()) return spelling.str();
      if (found->first != found->second && !rest.empty() &&
          !isPort(found->first, rest.split('.').first) &&
          !provesInternalNames(found->first, found->second)) {
        error = "dedup needs proved legacy internal identity renames for " + owner.str();
        return std::nullopt;
      }
      std::string suffix = rest.str();
      if (!rest.empty()) {
        auto root = rest.split('.').first;
        auto port = portNames[found->first].find(root.str());
        if (port != portNames[found->first].end())
          suffix = port->second + rest.drop_front(root.size()).str();
        else if (found->first != found->second) {
          auto named = mappedReference(found->first, root);
          if (!named) {
            error = "dedup target has no proved legacy internal reference: " + owner.str() + "." + root.str();
            return std::nullopt;
          }
          suffix = *named + rest.drop_front(root.size()).str();
        }
      }
      return circuitName.str() + "." + found->second + (rest.empty() ? "" : "." + suffix);
    }
    auto [circuitName, local] = spelling.split('|');
    if (local.empty()) return spelling.str();
    std::string result = circuitName.str() + "|";
    auto ownerEnd = local.find_first_of("/>");
    StringRef owner = local.take_front(ownerEnd);
    auto found = mapping.find(owner.str());
    if (found == mapping.end()) return spelling.str();
    std::string lastOld = owner.str(), lastNew = found->second;
    result += lastNew;
    StringRef tail = ownerEnd == StringRef::npos ? StringRef() : local.drop_front(ownerEnd);
    while (tail.consume_front("/")) {
      auto [instance, following] = tail.split(':');
      if (following.empty()) { error = "dedup target has a malformed instance path"; return std::nullopt; }
      auto childEnd = following.find_first_of("/>");
      auto child = following.take_front(childEnd);
      auto peer = mapping.find(child.str());
      if (peer == mapping.end()) { error = "dedup target has an unknown child module"; return std::nullopt; }
      auto named = instanceNames[lastOld].find(instance.str());
      if (named == instanceNames[lastOld].end()) {
        error = "dedup target has an unknown instance identity"; return std::nullopt;
      }
      if (lastOld != lastNew && !provesInternalNames(lastOld, lastNew)) {
        error = "dedup needs proved instance identity renames for " + lastOld; return std::nullopt;
      }
      result += "/" + named->second + ":" + peer->second;
      lastOld = child.str(); lastNew = peer->second;
      tail = childEnd == StringRef::npos ? StringRef() : following.drop_front(childEnd);
    }
    if (tail.consume_front(">")) {
      auto reference = tail.take_front(tail.find_first_of(".["));
      bool port = isPort(lastOld, reference);
      if (lastOld != lastNew && !port &&
          !provesInternalNames(lastOld, lastNew)) {
        error = "dedup needs explicit reference identity renames for " + lastOld + ">" + reference.str();
        return std::nullopt;
      }
      result += ">";
      if (port)
        result += portNames[lastOld].at(reference.str()) + tail.drop_front(reference.size()).str();
      else if (lastOld != lastNew) {
        auto named = references[lastOld].find(reference.str());
        if (named == references[lastOld].end()) {
          error = "dedup target has no proved internal reference: " + lastOld + ">" + reference.str();
          return std::nullopt;
        }
        result += named->second + tail.drop_front(reference.size()).str();
      } else
        result += tail.str();
    } else if (!tail.empty()) { error = "dedup target has unsupported trailing tokens"; return std::nullopt; }
    return result;
  };
  const std::set<std::string> known{
      "firrtl.transforms.DedupGroupAnnotation", "firrtl.transforms.NoDedupAnnotation",
      "firrtl.transforms.DontTouchAnnotation", "firrtl.passes.InlineAnnotation",
      "firrtl.transforms.BlackBoxInlineAnno", "chisel3.experimental.EnumAnnotations$EnumVecAnnotation",
      "chisel3.util.experimental.decode.DecodeTableAnnotation",
      goldengate::AnnotationClasses::MemModel.str(), goldengate::AnnotationClasses::FAMEModel.str(),
      goldengate::AnnotationClasses::EnableModelMultiThreading.str(),
      goldengate::AnnotationClasses::ChannelConnection.str(),
      goldengate::AnnotationClasses::HostClock.str(), goldengate::AnnotationClasses::HostReset.str(),
      goldengate::AnnotationClasses::BridgeIO.str()};
  SmallVector<Attribute> rewritten;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    auto kind = annotation.getClass();
    bool supported = known.count(kind.str());
    bool channel = annotation.isClass(goldengate::AnnotationClasses::ChannelConnection);
    bool bad = false;
    std::function<Attribute(Attribute, bool)> rewrite = [&](Attribute value, bool targetField) -> Attribute {
      if (auto text = dyn_cast<StringAttr>(value)) {
        auto renamed = renameTarget(text.getValue(), targetField);
        if (!renamed) { bad = true; return value; }
        if (*renamed != text.getValue()) {
          if (!supported || (!targetField && !channel)) { error = "dedup cannot retarget unknown annotation schema: " + kind.str(); bad = true; return value; }
          return StringAttr::get(staged.getContext(), *renamed);
        }
        return value;
      }
      if (auto array = dyn_cast<ArrayAttr>(value)) {
        SmallVector<Attribute> values;
        for (Attribute member : array) values.push_back(rewrite(member, targetField));
        return ArrayAttr::get(staged.getContext(), values);
      }
      if (auto dict = dyn_cast<DictionaryAttr>(value)) {
        SmallVector<NamedAttribute> values;
        for (NamedAttribute member : dict)
          values.emplace_back(member.getName(), rewrite(member.getValue(), member.getName() == "target"));
        return DictionaryAttr::get(staged.getContext(), values);
      }
      return value;
    };
    rewritten.push_back(rewrite(attr, false));
    if (bad) return failure();
  }
  staged->setAttr("rawAnnotations", ArrayAttr::get(staged.getContext(), rewritten));
  removed = before.size() - after.size();
  if (failed(verify(staged))) return reject("dedup produced invalid FIRRTL IR");
  return success();
}
} // namespace
LogicalResult goldengate::deduplicateModels(CircuitOp circuit, unsigned &removed,
                                            std::string &error) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  unsigned count = 0;
  if (failed(apply(circuit, *staged, count, error))) return failure();
  circuit->setAttrs((*staged)->getAttrs());
  circuit.getBody().takeBody(staged->getBody());
  removed = count;
  return success();
}
