// See LICENSE for license details.
#include "goldengate/BridgeAnalysis.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include <functional>
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

std::optional<llvm::SmallVector<goldengate::GGBridgeInstance>>
goldengate::analyzeBridgeInstances(CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "bridge analysis needs retained annotations";
    return std::nullopt;
  }
  std::map<std::string, FModuleLike> modules;
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations()) {
    auto module = dyn_cast<FModuleLike>(&op);
    if (!module)
      continue;
    modules.emplace(module.getModuleName().str(), module);
    if (module.getModuleName() == circuit.getName())
      top = dyn_cast<FModuleOp>(&op);
  }
  if (!top) {
    error = "bridge analysis needs an internal circuit top";
    return std::nullopt;
  }

  std::map<std::string, Annotation> bridgeAnnotations;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::Bridge))
      continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (!target) {
      error = "BridgeAnnotation has no module target";
      return std::nullopt;
    }
    auto resolved = resolveAnnotationTarget(circuit, target.getValue(), error);
    if (!resolved || resolved->port)
      return std::nullopt;
    auto moduleName = resolved->module.getModuleName().str();
    if (!bridgeAnnotations.emplace(moduleName, annotation).second) {
      error = "duplicate BridgeAnnotation for module " + moduleName;
      return std::nullopt;
    }
  }

  llvm::SmallVector<GGBridgeInstance> result;
  std::map<std::string, unsigned> instancesByModule;
  llvm::DenseSet<Operation *> activeModules;
  std::vector<std::string> path;
  std::function<bool(FModuleOp)> visit = [&](FModuleOp parent) {
    if (!activeModules.insert(parent.getOperation()).second) {
      error = "recursive instance hierarchy at " + parent.getName().str();
      return false;
    }
    for (InstanceOp instance : parent.getOps<InstanceOp>()) {
      path.push_back(instance.getName().str());
      auto moduleName = instance.getModuleName().str();
      auto module = modules.find(moduleName);
      if (module == modules.end()) {
        error = "instance refers to missing module " + moduleName;
        return false;
      }
      auto bridge = bridgeAnnotations.find(moduleName);
      if (bridge != bridgeAnnotations.end()) {
        auto widget = bridge->second.getMember<StringAttr>("widgetClass");
        auto channels = bridge->second.getMember<ArrayAttr>("bridgeChannels");
        if (!widget || !channels) {
          error = "bridge module has no widget class or channels: " + moduleName;
          return false;
        }
        GGBridgeInstance discovered{instance, module->second, path,
                                    widget.getValue().str(), {},
                                    bridge->second.getDict()};
        for (Attribute attr : channels) {
          auto channel = dyn_cast<DictionaryAttr>(attr);
          auto kind = channel ? channel.getAs<StringAttr>("class") : StringAttr();
          if (!kind) {
            error = "bridge channel has no class: " + moduleName;
            return false;
          }
          auto appendName = [&](llvm::StringRef member) {
            auto name = channel.getAs<StringAttr>(member);
            if (!name || name.getValue().empty()) {
              error = "bridge channel has no " + member.str() + ": " +
                      moduleName;
              return false;
            }
            discovered.channelNames.push_back(name.getValue().str());
            return true;
          };
          if (kind.getValue() == "firesim.lib.bridgeutils.PipeBridgeChannel" ||
              kind.getValue() == "firesim.lib.bridgeutils.ClockBridgeChannel") {
            if (!appendName("name"))
              return false;
          } else if (kind.getValue() ==
                     "firesim.lib.bridgeutils.ReadyValidBridgeChannel") {
            if (!appendName("fwdName") || !appendName("revName"))
              return false;
          } else {
            error = "unsupported bridge channel class: " +
                    kind.getValue().str();
            return false;
          }
        }
        ++instancesByModule[moduleName];
        result.push_back(std::move(discovered));
      }
      if (auto child = dyn_cast<FModuleOp>(module->second.getOperation()))
        if (!visit(child))
          return false;
      path.pop_back();
    }
    activeModules.erase(parent.getOperation());
    return true;
  };
  if (!visit(top))
    return std::nullopt;
  for (const auto &[moduleName, annotation] : bridgeAnnotations)
    if (!instancesByModule.count(moduleName)) {
      error = "annotated bridge module has no reachable instance: " +
              moduleName;
      return std::nullopt;
    }
  unsigned clockBridges = 0;
  for (const auto &bridge : result)
    clockBridges += bridge.widgetClass == "midas.widgets.ClockBridgeModule";
  if (!result.empty() && clockBridges != 1) {
    error = "expected exactly one ClockBridge instance";
    return std::nullopt;
  }
  return result;
}

mlir::LogicalResult goldengate::promoteBridgePorts(
    CircuitOp circuit, std::string &error, bool includeAggregates) {
  auto bridges = analyzeBridgeInstances(circuit, error);
  if (!bridges)
    return failure();
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op))
      if (module.getName() == circuit.getName())
        top = module;
  if (!top) {
    error = "bridge promotion needs an internal top";
    return failure();
  }
  std::set<std::string> portNames;
  for (const PortInfo &port : top.getPorts())
    portNames.insert(port.getName().str());
  SmallVector<Attribute> promotedAnnotations;
  if (includeAggregates)
    for (Attribute attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
      if (!Annotation(attr).isClass(AnnotationClasses::Bridge))
        promotedAnnotations.push_back(attr);
  std::function<bool(FIRRTLBaseType)> supportedType =
      [&](FIRRTLBaseType type) {
        if (isa<ClockType, UIntType, SIntType, ResetType, AsyncResetType>(type))
          return true;
        if (auto vector = dyn_cast<FVectorType>(type))
          return includeAggregates && supportedType(vector.getElementType());
        auto bundle = dyn_cast<BundleType>(type);
        if (!includeAggregates || !bundle)
          return false;
        for (const auto &element : bundle.getElements())
          if (!supportedType(element.type))
            return false;
        return true;
      };
  for (GGBridgeInstance bridge : *bridges) {
    if (bridge.path.size() != 1 ||
        bridge.instance->getBlock() != top.getBodyBlock()) {
      if (includeAggregates) {
        error = "bridge promotion needs a direct top instance: " +
                bridge.instance.getName().str();
        return failure();
      }
      continue;
    }
    SmallVector<BundleType::BundleElement> fields;
    std::map<unsigned, SmallVector<StringAttr>> vectorFields;
    std::map<unsigned, SmallVector<StringAttr>> bundleFields;
    std::map<unsigned, SmallVector<SmallVector<StringAttr>>> nestedBundleFields;
    std::set<unsigned> recursiveBundlePorts;
    std::set<std::string> fieldNames;
    bool supported = true;
    for (unsigned i = 0; i < bridge.module.getNumPorts(); ++i) {
      PortInfo port = bridge.module.getPorts()[i];
      auto type = dyn_cast<FIRRTLBaseType>(port.type);
      if (!type || !supportedType(type)) {
        supported = false;
        break;
      }
      // The Scala boundary lowers a vector of ground bridge signals into
      // numbered fields. Replace its constant-index users directly with
      // subfields of the promoted top port.
      if (includeAggregates)
        if (auto vector = dyn_cast<FVectorType>(type))
          if (isa<ClockType, UIntType, SIntType, ResetType, AsyncResetType>(
                  vector.getElementType())) {
            for (Operation *user : bridge.instance.getResult(i).getUsers())
              if (!isa<SubindexOp>(user)) {
                error = "bridge vector has a nonconstant-index use: " +
                        bridge.instance.getName().str() + "." +
                        port.getName().str();
                return failure();
              }
            auto &names = vectorFields[i];
            for (unsigned element = 0; element < vector.getNumElements();
                 ++element) {
              std::string name = port.getName().str() + "_" +
                                 std::to_string(element);
              if (!fieldNames.insert(name).second) {
                error = "duplicate flattened bridge field: " + name;
                return failure();
              }
              auto attr = StringAttr::get(circuit.getContext(), name);
              names.push_back(attr);
              fields.emplace_back(attr,
                                  bridge.module.getPortDirection(i) ==
                                      Direction::In,
                                  vector.getElementType());
            }
            continue;
          }
      // Bridge extraction also flattens a bundle whose immediate members are
      // ground signals (for example UARTBridge.uart). The outer input/output
      // direction and the member flip together determine the new field flip.
      if (includeAggregates)
        if (auto bundle = dyn_cast<BundleType>(type)) {
          bool groundMembers = true;
          bool nestedGroundMembers = true;
          for (const auto &element : bundle.getElements())
            groundMembers &= isa<ClockType, UIntType, SIntType, ResetType,
                                 AsyncResetType>(element.type);
          for (const auto &element : bundle.getElements()) {
            auto child = dyn_cast<BundleType>(element.type);
            if (!child) {
              nestedGroundMembers = false;
              break;
            }
            for (const auto &leaf : child.getElements())
              nestedGroundMembers &= isa<ClockType, UIntType, SIntType,
                                         ResetType, AsyncResetType>(leaf.type);
          }
          if (groundMembers) {
            for (Operation *user : bridge.instance.getResult(i).getUsers())
              if (!isa<SubfieldOp>(user)) {
                error = "bridge bundle has a whole-value use: " +
                        bridge.instance.getName().str() + "." +
                        port.getName().str();
                return failure();
              }
            auto &names = bundleFields[i];
            for (const auto &element : bundle.getElements()) {
              std::string name = port.getName().str() + "_" +
                                 element.name.getValue().str();
              if (!fieldNames.insert(name).second) {
                error = "duplicate flattened bridge field: " + name;
                return failure();
              }
              auto attr = StringAttr::get(circuit.getContext(), name);
              names.push_back(attr);
              fields.emplace_back(
                  attr,
                  (bridge.module.getPortDirection(i) == Direction::In) !=
                      element.isFlip,
                  element.type);
            }
            continue;
          }
          if (nestedGroundMembers) {
            for (Operation *user : bridge.instance.getResult(i).getUsers()) {
              auto outer = dyn_cast<SubfieldOp>(user);
              if (!outer) {
                error = "bridge bundle has a whole-value use: " +
                        bridge.instance.getName().str() + "." +
                        port.getName().str();
                return failure();
              }
              for (Operation *childUser : outer.getResult().getUsers())
                if (!isa<SubfieldOp>(childUser)) {
                  error = "bridge nested bundle has a whole-value use: " +
                          bridge.instance.getName().str() + "." +
                          port.getName().str() + "." +
                          outer.getFieldName().str();
                  return failure();
                }
            }
            auto &outerNames = nestedBundleFields[i];
            for (const auto &element : bundle.getElements()) {
              auto child = cast<BundleType>(element.type);
              SmallVector<StringAttr> leafNames;
              for (const auto &leaf : child.getElements()) {
                std::string name = port.getName().str() + "_" +
                                   element.name.getValue().str() + "_" +
                                   leaf.name.getValue().str();
                if (!fieldNames.insert(name).second) {
                  error = "duplicate flattened bridge field: " + name;
                  return failure();
                }
                auto attr = StringAttr::get(circuit.getContext(), name);
                leafNames.push_back(attr);
                fields.emplace_back(
                    attr,
                    ((bridge.module.getPortDirection(i) == Direction::In) !=
                     element.isFlip) != leaf.isFlip,
                    leaf.type);
              }
              outerNames.push_back(std::move(leafNames));
            }
            continue;
          }
          // AXI bridge records mix two- and three-level field paths. Flatten
          // any bundle of ground leaves when every aggregate SSA use is a
          // subfield; a whole-value connection cannot be rewritten this way.
          std::function<bool(FIRRTLBaseType)> hasGroundLeaves =
              [&](FIRRTLBaseType nestedType) {
                if (isa<ClockType, UIntType, SIntType, ResetType,
                        AsyncResetType>(nestedType))
                  return true;
                if (auto vector = dyn_cast<FVectorType>(nestedType))
                  return hasGroundLeaves(vector.getElementType());
                auto nested = dyn_cast<BundleType>(nestedType);
                if (!nested || nested.getElements().empty())
                  return false;
                for (const auto &member : nested.getElements())
                  if (!hasGroundLeaves(member.type))
                    return false;
                return true;
              };
          if (hasGroundLeaves(type)) {
            std::function<bool(Value)> hasOnlySubfieldUses = [&](Value value) {
              if (isa<FVectorType>(value.getType())) {
                for (Operation *user : value.getUsers()) {
                  auto subindex = dyn_cast<SubindexOp>(user);
                  if (!subindex || !hasOnlySubfieldUses(subindex.getResult()))
                    return false;
                }
                return true;
              }
              if (!isa<BundleType>(value.getType()))
                return true;
              for (Operation *user : value.getUsers()) {
                auto subfield = dyn_cast<SubfieldOp>(user);
                if (!subfield || !hasOnlySubfieldUses(subfield.getResult()))
                  return false;
              }
              return true;
            };
            if (!hasOnlySubfieldUses(bridge.instance.getResult(i))) {
              error = "bridge bundle has a whole-value use: " +
                      bridge.instance.getName().str() + "." +
                      port.getName().str();
              return failure();
            }
            std::function<bool(FIRRTLBaseType, std::string, bool)> addLeaves =
                [&](FIRRTLBaseType nestedType, std::string prefix, bool flip) {
                  if (auto vector = dyn_cast<FVectorType>(nestedType)) {
                    for (unsigned element = 0; element < vector.getNumElements();
                         ++element)
                      if (!addLeaves(vector.getElementType(),
                                     prefix + "_" + std::to_string(element), flip))
                        return false;
                    return true;
                  }
                  if (auto nested = dyn_cast<BundleType>(nestedType)) {
                    for (const auto &member : nested.getElements()) {
                    std::string name =
                        prefix + "_" + member.name.getValue().str();
                    bool memberFlip = flip != member.isFlip;
                    if (!addLeaves(member.type, name, memberFlip))
                        return false;
                    }
                    return true;
                  }
                  if (!fieldNames.insert(prefix).second) {
                    error = "duplicate flattened bridge field: " + prefix;
                    return false;
                  }
                  fields.emplace_back(StringAttr::get(circuit.getContext(), prefix),
                                      flip, nestedType);
                  return true;
                };
            if (!addLeaves(bundle, port.getName().str(),
                           bridge.module.getPortDirection(i) == Direction::In))
              return failure();
            recursiveBundlePorts.insert(i);
            continue;
          }
        }
      if (!fieldNames.insert(port.getName().str()).second) {
        error = "duplicate bridge field: " + port.getName().str();
        return failure();
      }
      fields.emplace_back(port.name,
                          bridge.module.getPortDirection(i) == Direction::In,
                          type);
    }
    if (!supported) {
      if (includeAggregates) {
        error = "bridge has unsupported port type: " +
                bridge.instance.getName().str();
        return failure();
      }
      continue;
    }
    std::string name = bridge.instance.getName().str();
    if (!portNames.insert(name).second) {
      error = "bridge port name collides: " + name;
      return failure();
    }
    unsigned index = top.getNumPorts();
    top.insertPorts({{index, PortInfo(StringAttr::get(circuit.getContext(), name),
                                      BundleType::get(circuit.getContext(), fields),
                                      Direction::In)}});
    Value bundle = top.getBodyBlock()->getArgument(index);
    OpBuilder builder(&top.getBodyBlock()->front());
    for (unsigned i = 0; i < bridge.module.getNumPorts(); ++i) {
      if (auto found = vectorFields.find(i); found != vectorFields.end()) {
        SmallVector<SubindexOp> users;
        for (Operation *user : bridge.instance.getResult(i).getUsers())
          users.push_back(cast<SubindexOp>(user));
        for (SubindexOp subindex : users) {
          Value field = builder.create<SubfieldOp>(
              subindex.getLoc(), bundle, found->second[subindex.getIndex()]);
          subindex.getResult().replaceAllUsesWith(field);
          subindex.erase();
        }
      } else if (auto found = bundleFields.find(i);
                 found != bundleFields.end()) {
        SmallVector<SubfieldOp> users;
        for (Operation *user : bridge.instance.getResult(i).getUsers())
          users.push_back(cast<SubfieldOp>(user));
        for (SubfieldOp subfield : users) {
          Value field = builder.create<SubfieldOp>(
              subfield.getLoc(), bundle, found->second[subfield.getFieldIndex()]);
          subfield.getResult().replaceAllUsesWith(field);
          subfield.erase();
        }
      } else if (auto found = nestedBundleFields.find(i);
                 found != nestedBundleFields.end()) {
        SmallVector<SubfieldOp> outerUsers;
        for (Operation *user : bridge.instance.getResult(i).getUsers())
          outerUsers.push_back(cast<SubfieldOp>(user));
        for (SubfieldOp outer : outerUsers) {
          SmallVector<SubfieldOp> leafUsers;
          for (Operation *user : outer.getResult().getUsers())
            leafUsers.push_back(cast<SubfieldOp>(user));
          for (SubfieldOp leaf : leafUsers) {
            Value field = builder.create<SubfieldOp>(
                leaf.getLoc(), bundle,
                found->second[outer.getFieldIndex()][leaf.getFieldIndex()]);
            leaf.getResult().replaceAllUsesWith(field);
            leaf.erase();
          }
          outer.erase();
        }
      } else if (recursiveBundlePorts.count(i)) {
        std::function<void(Value, std::string)> replaceLeaves =
            [&](Value value, std::string prefix) {
              if (isa<FVectorType>(value.getType())) {
                SmallVector<SubindexOp> users;
                for (Operation *user : value.getUsers())
                  users.push_back(cast<SubindexOp>(user));
                for (SubindexOp subindex : users) {
                  std::string fieldName =
                      prefix + "_" + std::to_string(subindex.getIndex());
                  if (isa<BundleType, FVectorType>(subindex.getResult().getType()))
                    replaceLeaves(subindex.getResult(), fieldName);
                  else
                    subindex.getResult().replaceAllUsesWith(
                        builder.create<SubfieldOp>(
                            subindex.getLoc(), bundle,
                            StringAttr::get(circuit.getContext(), fieldName)));
                  subindex.erase();
                }
                return;
              }
              SmallVector<SubfieldOp> users;
              for (Operation *user : value.getUsers())
                users.push_back(cast<SubfieldOp>(user));
              for (SubfieldOp subfield : users) {
                std::string name = prefix + "_" + subfield.getFieldName().str();
                if (isa<BundleType, FVectorType>(subfield.getResult().getType())) {
                  replaceLeaves(subfield.getResult(), name);
                } else {
                  Value field = builder.create<SubfieldOp>(
                      subfield.getLoc(), bundle,
                      StringAttr::get(circuit.getContext(), name));
                  subfield.getResult().replaceAllUsesWith(field);
                }
                subfield.erase();
              }
            };
        replaceLeaves(bridge.instance.getResult(i),
                      bridge.module.getPortName(i).str());
      } else {
        bridge.instance.getResult(i).replaceAllUsesWith(
            builder.create<SubfieldOp>(bridge.instance.getLoc(), bundle,
                                       bridge.module.getPortName(i)));
      }
    }
    bridge.instance.erase();
    if (includeAggregates) {
      auto constructorKey =
          Annotation(bridge.sourceAnnotation)
              .getMember<DictionaryAttr>("widgetConstructorKey");
      if (!constructorKey) {
        error = "bridge has no widget constructor key: " + name;
        return failure();
      }
      SmallVector<NamedAttribute> channelMapping;
      for (const std::string &channel : bridge.channelNames)
        channelMapping.emplace_back(
            StringAttr::get(circuit.getContext(), channel),
            StringAttr::get(circuit.getContext(), name + "_" + channel));
      auto string = [&](llvm::StringRef value) {
        return StringAttr::get(circuit.getContext(), value);
      };
      promotedAnnotations.push_back(DictionaryAttr::get(
          circuit.getContext(),
          {NamedAttribute(string("class"), string(AnnotationClasses::BridgeIO)),
           NamedAttribute(string("target"),
                          string("~" + circuit.getName().str() + "|" +
                                 circuit.getName().str() + ">" + name)),
           NamedAttribute(string("channelMapping"),
                          DictionaryAttr::get(circuit.getContext(),
                                              channelMapping)),
           NamedAttribute(string("widgetClass"), string(bridge.widgetClass)),
           NamedAttribute(string("widgetConstructorKey"), constructorKey)}));

      // BridgeExtraction's PipeBridgeChannel connections refer to fields of
      // the newly promoted top port. Resolve each original module-local field
      // through the actual flattened port fields before emitting its target.
      auto sourceChannels = Annotation(bridge.sourceAnnotation)
                                .getMember<ArrayAttr>("bridgeChannels");
      if (!sourceChannels) {
        error = "bridge has no channels: " + name;
        return failure();
      }
      auto retarget = [&](StringAttr original) -> std::optional<StringAttr> {
        if (!original)
          return std::nullopt;
        std::string prefix = "~" + circuit.getName().str() + "|" +
                             bridge.module.getModuleName().str() + ">";
        llvm::StringRef spelling = original.getValue();
        if (!spelling.consume_front(prefix))
          return std::nullopt;
        std::string field;
        for (size_t i = 0; i < spelling.size(); ++i) {
          char character = spelling[i];
          if (character == '.')
            field.push_back('_');
          else if (character == '[') {
            field.push_back('_');
            while (++i < spelling.size() && spelling[i] != ']') {
              if (spelling[i] < '0' || spelling[i] > '9')
                return std::nullopt;
              field.push_back(spelling[i]);
            }
            if (i == spelling.size())
              return std::nullopt;
          } else if (character == ']' || character == '>' ||
                     character == '|') {
            return std::nullopt;
          } else {
            field.push_back(character);
          }
        }
        if (!fieldNames.count(field))
          return std::nullopt;
        return string("~" + circuit.getName().str() + "|" +
                      circuit.getName().str() + ">" + name + "." + field);
      };
      for (Attribute attr : sourceChannels) {
        auto channel = dyn_cast<DictionaryAttr>(attr);
        auto channelClass =
            channel ? channel.getAs<StringAttr>("class") : StringAttr();
        if (!channelClass)
          continue;
        if (channelClass.getValue() ==
            "firesim.lib.bridgeutils.ClockBridgeChannel") {
          auto channelName = channel.getAs<StringAttr>("name");
          auto sinks = channel.getAs<ArrayAttr>("sinks");
          auto clocks = channel.getAs<ArrayAttr>("clocks");
          auto mfmrs = channel.getAs<ArrayAttr>("clockMFMRs");
          if (!channelName || !sinks || !clocks || !mfmrs ||
              sinks.empty() || sinks.size() != clocks.size() ||
              clocks.size() != mfmrs.size()) {
            error = "malformed clock bridge channel in " + name;
            return failure();
          }
          SmallVector<Attribute> sinkTargets;
          for (Attribute endpoint : sinks) {
            auto target = retarget(dyn_cast<StringAttr>(endpoint));
            if (!target) {
              error = "unresolved sink in clock bridge channel " +
                      channelName.getValue().str();
              return failure();
            }
            sinkTargets.push_back(*target);
          }
          auto clockInfo = DictionaryAttr::get(
              circuit.getContext(),
              {{string("class"), string(AnnotationClasses::TargetClockChannel)},
               {string("clockInfo"), clocks},
               {string("perClockMFMR"), mfmrs}});
          promotedAnnotations.push_back(DictionaryAttr::get(
              circuit.getContext(),
              {{string("class"), string(AnnotationClasses::ChannelConnection)},
               {string("globalName"),
                string(name + "_" + channelName.getValue().str())},
               {string("channelInfo"), clockInfo},
               {string("sinks"),
                ArrayAttr::get(circuit.getContext(), sinkTargets)}}));
          continue;
        }
        if (channelClass.getValue() ==
            "firesim.lib.bridgeutils.ReadyValidBridgeChannel") {
          auto forwardName = channel.getAs<StringAttr>("fwdName");
          auto reverseName = channel.getAs<StringAttr>("revName");
          auto clock = retarget(channel.getAs<StringAttr>("clock"));
          auto valid = retarget(channel.getAs<StringAttr>("valid"));
          auto ready = retarget(channel.getAs<StringAttr>("ready"));
          auto sinks = channel.getAs<ArrayAttr>("sinks");
          auto sources = channel.getAs<ArrayAttr>("sources");
          if (!forwardName || !reverseName || !clock || !valid || !ready ||
              !sinks || !sources || sinks.empty() == sources.empty()) {
            error = "malformed ready/valid bridge channel in " + name;
            return failure();
          }
          bool sourceSide = sinks.empty();
          SmallVector<Attribute> forwardTargets;
          for (Attribute endpoint : sourceSide ? sources : sinks) {
            auto target = retarget(dyn_cast<StringAttr>(endpoint));
            if (!target) {
              error = "unresolved forward endpoint in ready/valid channel " +
                      forwardName.getValue().str();
              return failure();
            }
            forwardTargets.push_back(*target);
          }
          auto forwardInfo = DictionaryAttr::get(
              circuit.getContext(),
              {{string("class"),
                string(AnnotationClasses::DecoupledForwardChannel)},
               {string(sourceSide ? "validSource" : "validSink"), *valid},
               {string(sourceSide ? "readySink" : "readySource"), *ready}});
          promotedAnnotations.push_back(DictionaryAttr::get(
              circuit.getContext(),
              {{string("class"), string(AnnotationClasses::ChannelConnection)},
               {string("globalName"),
                string(name + "_" + forwardName.getValue().str())},
               {string("channelInfo"), forwardInfo},
               {string("clock"), *clock},
               {string(sourceSide ? "sources" : "sinks"),
                ArrayAttr::get(circuit.getContext(), forwardTargets)}}));
          promotedAnnotations.push_back(DictionaryAttr::get(
              circuit.getContext(),
              {{string("class"), string(AnnotationClasses::ChannelConnection)},
               {string("globalName"),
                string(name + "_" + reverseName.getValue().str())},
               {string("channelInfo"),
                DictionaryAttr::get(
                    circuit.getContext(),
                    {{string("class"),
                      string(AnnotationClasses::DecoupledReverseChannel)}})},
               {string("clock"), *clock},
               {string(sourceSide ? "sinks" : "sources"),
                ArrayAttr::get(circuit.getContext(), {*ready})}}));
          continue;
        }
        if (channelClass.getValue() !=
            "firesim.lib.bridgeutils.PipeBridgeChannel")
          continue;
        auto channelName = channel.getAs<StringAttr>("name");
        auto latency = channel.getAs<IntegerAttr>("latency");
        auto clock = retarget(channel.getAs<StringAttr>("clock"));
        auto sinks = channel.getAs<ArrayAttr>("sinks");
        auto sources = channel.getAs<ArrayAttr>("sources");
        if (!channelName || !latency || !clock || !sinks || !sources) {
          error = "malformed pipe bridge channel in " + name;
          return failure();
        }
        SmallVector<NamedAttribute> connection{
            {string("class"), string(AnnotationClasses::ChannelConnection)},
            {string("globalName"), string(name + "_" + channelName.getValue().str())},
            {string("channelInfo"),
             DictionaryAttr::get(circuit.getContext(),
                                 {{string("class"), string(AnnotationClasses::PipeChannel)},
                                  {string("latency"), latency}})},
            {string("clock"), *clock}};
        for (auto [key, endpoints] :
             {std::pair<llvm::StringRef, ArrayAttr>{"sinks", sinks},
              {"sources", sources}}) {
          if (endpoints.empty())
            continue;
          SmallVector<Attribute> targets;
          for (Attribute endpoint : endpoints) {
            auto target = retarget(dyn_cast<StringAttr>(endpoint));
            if (!target) {
              error = "unresolved " + key.str() + " in pipe channel " +
                      channelName.getValue().str();
              return failure();
            }
            targets.push_back(*target);
          }
          connection.emplace_back(string(key),
                                  ArrayAttr::get(circuit.getContext(), targets));
        }
        promotedAnnotations.push_back(
            DictionaryAttr::get(circuit.getContext(), connection));
      }
    }
  }
  if (includeAggregates)
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(),
                                                       promotedAnnotations));
  return success();
}
