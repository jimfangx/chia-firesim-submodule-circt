// See LICENSE for license details.
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/AnnotationClasses.h"
#include "FAMEPortAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "llvm/ADT/BitVector.h"
#include "mlir/IR/Builders.h"
#include <algorithm>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
bool hasPortAnnotations(FModuleOp module, unsigned port) {
  auto annotations = module.getPortAnnotationsAttr();
  return annotations && port < annotations.size() &&
         !cast<ArrayAttr>(annotations[port]).empty();
}

llvm::StringRef removeCommonPrefix(llvm::StringRef name,
                                   llvm::StringRef channel) {
  while (!name.empty() && !channel.empty() && name.front() == channel.front()) {
    name = name.drop_front();
    channel = channel.drop_front();
  }
  return name;
}

LogicalResult rewriteMultiportOutputChannel(
    const goldengate::TopHierarchy &hierarchy,
    const goldengate::FAMETopChannelPort &channel, std::string &error) {
  const auto &binding = *channel.binding;
  auto top = hierarchy.top;
  auto modelModule = binding.portGroup->module;
  auto model = dyn_cast<FModuleOp>(modelModule.getOperation());
  auto instance = binding.instance;
  auto channelType = channel.type;
  auto bitsIndex = channelType.getElementIndex("bits");
  auto readyIndex = channelType.getElementIndex("ready");
  auto validIndex = channelType.getElementIndex("valid");
  auto payload = bitsIndex ? dyn_cast<BundleType>(
                                 channelType.getElements()[*bitsIndex].type)
                           : BundleType();
  if (!model || binding.portGroup->direction != Direction::Out ||
      binding.instancePorts.size() < 2 ||
      instance->getParentOfType<FModuleOp>() != top || !payload ||
      payload.getElements().size() != binding.instancePorts.size() ||
      !readyIndex || !validIndex || channelType.getElements().size() != 3 ||
      !channelType.getElements()[*readyIndex].isFlip ||
      channelType.getElements()[*validIndex].isFlip) {
    error = "expected a multiport bundle output on a top-level FAME model";
    return failure();
  }

  struct Leaf {
    unsigned modelPort;
    std::string field;
  };
  llvm::SmallVector<Leaf> leaves;
  llvm::SmallVector<std::pair<unsigned, Operation *>> aliasesToErase;
  llvm::SmallVector<Annotation> wrapperAnnotations;
  llvm::SmallVector<circt::hw::InnerSymPropertiesAttr> wrapperSymbols, modelSymbols;
  std::set<unsigned> topPorts;
  for (unsigned modelPort : binding.instancePorts) {
    auto modelName = model.getPortName(modelPort);
    auto field = removeCommonPrefix(modelName, binding.portGroup->name);
    auto fieldIndex = payload.getElementIndex(field);
    if (field.empty() || !fieldIndex ||
        payload.getElements()[*fieldIndex].isFlip ||
        payload.getElements()[*fieldIndex].type !=
            model.getPorts()[modelPort].type ||
        hasPortAnnotations(model, modelPort)) {
      error = "FAME output bundle field does not match a model port";
      return failure();
    }
    std::set<unsigned> aliases;
    for (const auto &connection : hierarchy.connections)
      if (connection.instance == instance &&
          connection.instancePort == modelPort)
        aliases.insert(connection.topPort);
    if (aliases.empty()) {
      error = "FAME output bundle field has no matching top port";
      return failure();
    }
    if (failed(goldengate::collectFAMEPayloadSymbols(
            model, modelPort, channel.type, field, modelSymbols, error)))
      return failure();
    llvm::SmallVector<Operation *> connections;
    Value oldInstance = instance.getResult(modelPort);
    for (unsigned topPort : aliases) {
      auto aliasField = goldengate::getFAMEOutputAliasField(
          hierarchy, channel, modelPort, topPort, error);
      if (!aliasField || *aliasField != field ||
          !topPorts.insert(topPort).second ||
          top.getPortDirection(topPort) != Direction::Out ||
          top.getPorts()[topPort].type != model.getPorts()[modelPort].type) {
        if (error.empty()) error = "FAME output bundle alias has incompatible payload";
        return failure();
      }
      if (failed(goldengate::collectFAMEWrapperPayloadMetadata(
              top, topPort, channel.type, field, wrapperAnnotations,
              wrapperSymbols, error)))
        return failure();
      Value oldTop = top.getBodyBlock()->getArgument(topPort);
      Operation *oldConnection = nullptr;
      for (OpOperand &use : oldTop.getUses()) {
        Operation *connect = use.getOwner();
        auto strict = dyn_cast<StrictConnectOp>(connect);
        auto ordinary = dyn_cast<ConnectOp>(connect);
        if ((!strict && !ordinary) ||
            (strict && (strict.getDest() != oldTop ||
                        strict.getSrc() != oldInstance)) ||
            (ordinary && (ordinary.getDest() != oldTop ||
                          ordinary.getSrc() != oldInstance)) || oldConnection) {
          error = "FAME output bundle field has nontrivial top wiring";
          return failure();
        }
        oldConnection = connect;
      }
      if (!oldConnection) {
        error = "FAME output bundle alias has no direct connection";
        return failure();
      }
      connections.push_back(oldConnection);
      aliasesToErase.push_back({topPort, oldConnection});
    }
    for (OpOperand &use : oldInstance.getUses())
      if (!llvm::is_contained(connections, use.getOwner())) {
        error = "FAME output bundle field has a use outside its top aliases";
        return failure();
      }
    leaves.push_back({modelPort, field.str()});
  }

  std::string modelName = binding.portGroup->name + "_source";
  for (const auto &port : model.getPorts())
    if (port.getName() == modelName) {
      error = "FAME model source port already exists";
      return failure();
    }
  for (const auto &port : top.getPorts())
    if (port.getName() == channel.portName) {
      error = "FAME top source port already exists";
      return failure();
    }

  unsigned modelInsert = *std::min_element(binding.instancePorts.begin(),
                                          binding.instancePorts.end());
  unsigned topInsert = *topPorts.begin();
  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelName), channel.type,
                     Direction::Out);
  PortInfo topInfo(StringAttr::get(context, channel.portName), channel.type,
                   Direction::Out);
  if (!modelSymbols.empty()) {
    llvm::sort(modelSymbols, [](auto a, auto b) {
      return a.getFieldID() < b.getFieldID();
    });
    modelInfo.sym = circt::hw::InnerSymAttr::get(context, modelSymbols);
  }
  topInfo.annotations = AnnotationSet(wrapperAnnotations, context);
  if (!wrapperSymbols.empty()) {
    llvm::sort(wrapperSymbols, [](auto a, auto b) {
      return a.getFieldID() < b.getFieldID();
    });
    topInfo.sym = circt::hw::InnerSymAttr::get(context, wrapperSymbols);
  }
  model.insertPorts({{modelInsert, modelInfo}});
  OpBuilder body(model.getBodyBlock(), model.getBodyBlock()->begin());
  Value bits = body.create<SubfieldOp>(
      model.getLoc(), model.getBodyBlock()->getArgument(modelInsert), "bits");
  for (const auto &leaf : leaves) {
    Value field = body.create<SubfieldOp>(model.getLoc(), bits, leaf.field);
    model.getBodyBlock()->getArgument(leaf.modelPort + 1)
        .replaceAllUsesWith(field);
  }
  llvm::BitVector eraseModel(model.getNumPorts());
  for (const auto &leaf : leaves)
    eraseModel.set(leaf.modelPort + 1);
  model.erasePorts(eraseModel);

  top.insertPorts({{topInsert, topInfo}});
  Value newTop = top.getBodyBlock()->getArgument(topInsert);
  for (auto &alias : aliasesToErase)
    alias.second->erase();
  llvm::BitVector eraseTop(top.getNumPorts());
  for (unsigned port : topPorts)
    eraseTop.set(port + 1);
  top.erasePorts(eraseTop);

  InstanceOp expanded = instance.cloneAndInsertPorts({{modelInsert, modelInfo}});
  std::set<unsigned> replacedPorts(binding.instancePorts.begin(),
                                   binding.instancePorts.end());
  for (unsigned i = 0, n = instance.getNumResults(); i < n; ++i) {
    if (replacedPorts.count(i))
      continue;
    unsigned newIndex = i + (i >= modelInsert);
    if (instance.getPortNameStr(i) != expanded.getPortNameStr(newIndex) ||
        instance.getResult(i).getType() != expanded.getResult(newIndex).getType()) {
      error = "FAME output bundle changed an unrelated instance port";
      return failure();
    }
    instance.getResult(i).replaceAllUsesWith(expanded.getResult(newIndex));
  }
  instance.erase();
  llvm::BitVector eraseInstance(expanded.getNumResults());
  for (const auto &leaf : leaves)
    eraseInstance.set(leaf.modelPort + 1);
  OpBuilder parentBuilder(expanded);
  InstanceOp replacement = expanded.erasePorts(parentBuilder, eraseInstance);
  expanded.erase();
  OpBuilder connectionBuilder(replacement);
  connectionBuilder.setInsertionPointAfter(replacement);
  connectionBuilder.create<ConnectOp>(
      replacement.getLoc(), newTop, replacement.getResult(modelInsert));
  return success();
}
} // namespace

// Match wrapper aliases through retained source annotations and the actual
// instance connections. The ordered producer identity must match in every
// branch, and SFC's branch-specific prefix removal must yield the same leaf.
std::optional<std::string> goldengate::getFAMEOutputAliasField(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    unsigned modelPort, unsigned topPort, std::string &error) {
  const auto &binding = *channel.binding;
  auto top = hierarchy.top;
  auto model = binding.portGroup->module;
  auto circuit = top->getParentOfType<CircuitOp>();
  auto target = [&](unsigned port) {
    return "~" + circuit.getName().str() + "|" + top.getName().str() +
           ">" + top.getPortName(port).str();
  };
  std::string expected = removeCommonPrefix(
      model.getPortName(modelPort),
      binding.portGroup->name).str();
  auto type = channel.type;
  auto bits = type.getElementIndex("bits");
  auto payload = bits ? dyn_cast<BundleType>(type.getElementType(*bits))
                      : BundleType();
  if (!payload || payload.getElements().size() != binding.instancePorts.size()) {
    error = "FAME output alias has no ordered bundle payload";
    return std::nullopt;
  }
  // ModelChannelBinding stores sorted physical indices. Payload fields retain
  // source annotation order, which is the producer order used by SFC dedup.
  llvm::SmallVector<unsigned> orderedPorts;
  for (const auto &leaf : payload.getElements()) {
    auto port = llvm::find_if(binding.instancePorts, [&](unsigned index) {
      return removeCommonPrefix(model.getPortName(index), binding.portGroup->name) ==
             leaf.name.getValue();
    });
    if (port == binding.instancePorts.end()) {
      error = "FAME output alias payload leaf has no model port";
      return std::nullopt;
    }
    orderedPorts.push_back(*port);
  }
  bool covered = false;
  if (auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
    for (auto attr : annotations) {
      Annotation annotation(attr);
      if (!annotation.isClass(AnnotationClasses::ChannelConnection)) continue;
      auto sources = annotation.getMember<ArrayAttr>("sources");
      if (!sources || !llvm::any_of(sources, [&](Attribute source) {
            auto value = dyn_cast<StringAttr>(source);
            return value && value.getValue() == target(topPort);
          })) continue;
      auto name = annotation.getMember<StringAttr>("globalName");
      if (!name || sources.size() != binding.instancePorts.size()) {
        error = "FAME output alias is not a complete ordered channel source";
        return std::nullopt;
      }
      for (unsigned i = 0; i < sources.size(); ++i) {
        auto source = dyn_cast<StringAttr>(sources[i]);
        bool matched = false;
        for (const auto &connection : hierarchy.connections) {
          if (!source || connection.instance != binding.instance ||
              connection.instancePort != orderedPorts[i] ||
              source.getValue() != target(connection.topPort)) continue;
          auto field = removeCommonPrefix(top.getPortName(connection.topPort),
                                          name.getValue());
          auto modelField = removeCommonPrefix(
              model.getPortName(orderedPorts[i]),
              binding.portGroup->name);
          matched |= !field.empty() && field == modelField;
        }
        if (!matched) {
          error = "FAME output alias branch differs in producer order or payload fields";
          return std::nullopt;
        }
      }
      covered = true;
    }
  unsigned aliases = 0;
  for (const auto &connection : hierarchy.connections)
    aliases += connection.instance == binding.instance &&
               connection.instancePort == modelPort;
  if (!covered && (aliases > 1 ||
      removeCommonPrefix(top.getPortName(topPort), binding.globalName) != expected)) {
    error = "FAME output alias is not an annotated bundle channel source";
    return std::nullopt;
  }
  return expected;
}

LogicalResult goldengate::rewriteFAMEOutputChannel(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    std::string &error) {
  const auto &binding = *channel.binding;
  auto top = hierarchy.top;
  auto modelModule = binding.portGroup->module;
  auto model = dyn_cast<FModuleOp>(modelModule.getOperation());
  auto instance = binding.instance;
  auto channelType = channel.type;
  if (binding.instancePorts.size() > 1)
    return rewriteMultiportOutputChannel(hierarchy, channel, error);
  if (!model || binding.portGroup->direction != Direction::Out ||
      binding.instancePorts.size() != 1 ||
      instance->getParentOfType<FModuleOp>() != top) {
    error = "expected one scalar output on a top-level FAME model instance";
    return failure();
  }
  unsigned modelPort = binding.instancePorts.front();
  auto payloadType = dyn_cast<FIRRTLBaseType>(model.getPorts()[modelPort].type);
  auto bitsIndex = channelType.getElementIndex("bits");
  auto readyIndex = channelType.getElementIndex("ready");
  auto validIndex = channelType.getElementIndex("valid");
  if (!payloadType || isa<BundleType, FVectorType>(payloadType) ||
      !bitsIndex || !readyIndex || !validIndex ||
      channel.type.getElements().size() != 3 ||
      channel.type.getElements()[*bitsIndex].type != payloadType ||
      !channel.type.getElements()[*readyIndex].isFlip ||
      channel.type.getElements()[*validIndex].isFlip ||
      hasPortAnnotations(model, modelPort)) {
    error = "FAME output channel payload or port annotations are incompatible";
    return failure();
  }
  std::string modelName = binding.portGroup->name + "_source";
  for (const auto &port : model.getPorts())
    if (port.getName() == modelName) {
      error = "FAME model source port already exists";
      return failure();
    }

  std::set<unsigned> topPorts;
  for (const auto &connection : hierarchy.connections)
    if (connection.instance == instance &&
        connection.instancePort == modelPort)
      topPorts.insert(connection.topPort);
  if (topPorts.empty()) {
    error = "FAME output channel has no matching top port";
    return failure();
  }
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    if (top.getPortName(i) == channel.portName && !topPorts.count(i)) {
      error = "FAME top source port already exists";
      return failure();
    }
  llvm::SmallVector<Annotation> wrapperAnnotations;
  llvm::SmallVector<circt::hw::InnerSymPropertiesAttr> wrapperSymbols, modelSymbols;
  llvm::SmallVector<Operation *> oldConnections;
  Value oldInstance = instance.getResult(modelPort);
  auto circuit = top->getParentOfType<CircuitOp>();
  for (unsigned topPort : topPorts) {
    if (top.getPortDirection(topPort) != Direction::Out ||
        top.getPorts()[topPort].type != payloadType) {
      error = "FAME output alias has incompatible direction or payload";
      return failure();
    }
    // SFC only removes connections whose old top targets are channel sources.
    // An unannotated alias is a surviving output, not part of this producer.
    if (topPorts.size() > 1) {
      std::string target = "~" + circuit.getName().str() + "|" +
                           top.getName().str() + ">" +
                           top.getPortName(topPort).str();
      bool covered = false;
      if (auto annotations = circuit->getAttrOfType<ArrayAttr>("rawAnnotations"))
        for (auto attr : annotations) {
          Annotation annotation(attr);
          if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
            continue;
          auto sources = annotation.getMember<ArrayAttr>("sources");
          auto info = annotation.getMember<DictionaryAttr>("channelInfo");
          auto kind = info ? info.getAs<StringAttr>("class") : StringAttr();
          if (!sources || sources.size() != 1 || !kind ||
              (kind.getValue() != goldengate::AnnotationClasses::PipeChannel &&
               kind.getValue() != goldengate::AnnotationClasses::DecoupledForwardChannel &&
               kind.getValue() != goldengate::AnnotationClasses::DecoupledReverseChannel))
            continue;
          auto source = dyn_cast<StringAttr>(sources[0]);
          covered |= source && source.getValue() == target;
        }
      if (!covered) {
        error = "FAME output alias is not an annotated scalar channel source";
        return failure();
      }
    }
    if (failed(collectFAMEWrapperPayloadMetadata(
            top, topPort, channel.type, {}, wrapperAnnotations,
            wrapperSymbols, error)))
      return failure();
    Value oldTop = top.getBodyBlock()->getArgument(topPort);
    Operation *oldConnection = nullptr;
    for (OpOperand &use : oldTop.getUses()) {
      Operation *connect = use.getOwner();
      auto strict = dyn_cast<StrictConnectOp>(connect);
      auto ordinary = dyn_cast<ConnectOp>(connect);
      if ((!strict && !ordinary) ||
          (strict && (strict.getDest() != oldTop ||
                      strict.getSrc() != oldInstance)) ||
          (ordinary && (ordinary.getDest() != oldTop ||
                        ordinary.getSrc() != oldInstance)) || oldConnection) {
        error = "FAME output channel has nontrivial top wiring";
        return failure();
      }
      oldConnection = connect;
    }
    if (!oldConnection) {
      error = "FAME output alias has no direct connection";
      return failure();
    }
    oldConnections.push_back(oldConnection);
  }
  for (OpOperand &use : oldInstance.getUses())
    if (!llvm::is_contained(oldConnections, use.getOwner())) {
      error = "FAME output channel has a use outside its top aliases";
      return failure();
    }
  if (failed(collectFAMEPayloadSymbols(
          model, modelPort, channel.type, {}, modelSymbols, error)))
    return failure();
  unsigned topInsert = *topPorts.begin();

  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelName), channel.type,
                     Direction::Out);
  PortInfo topInfo(StringAttr::get(context, channel.portName), channel.type,
                   Direction::Out);
  if (!modelSymbols.empty()) {
    llvm::sort(modelSymbols, [](auto a, auto b) {
      return a.getFieldID() < b.getFieldID();
    });
    modelInfo.sym = circt::hw::InnerSymAttr::get(context, modelSymbols);
  }
  topInfo.annotations = AnnotationSet(wrapperAnnotations, context);
  if (!wrapperSymbols.empty()) {
    llvm::sort(wrapperSymbols, [](auto a, auto b) {
      return a.getFieldID() < b.getFieldID();
    });
    topInfo.sym = circt::hw::InnerSymAttr::get(context, wrapperSymbols);
  }
  model.insertPorts({{modelPort, modelInfo}});
  Value oldModel = model.getBodyBlock()->getArgument(modelPort + 1);
  OpBuilder body(model.getBodyBlock(), model.getBodyBlock()->begin());
  Value bits = body.create<SubfieldOp>(model.getLoc(),
      model.getBodyBlock()->getArgument(modelPort), "bits");
  oldModel.replaceAllUsesWith(bits);
  llvm::BitVector eraseModel(model.getNumPorts());
  eraseModel.set(modelPort + 1);
  model.erasePorts(eraseModel);

  top.insertPorts({{topInsert, topInfo}});
  Value newTop = top.getBodyBlock()->getArgument(topInsert);
  for (auto *connection : oldConnections)
    connection->erase();
  llvm::BitVector eraseTop(top.getNumPorts());
  for (unsigned port : topPorts)
    eraseTop.set(port + 1);
  top.erasePorts(eraseTop);

  InstanceOp expanded = instance.cloneAndInsertPorts({{modelPort, modelInfo}});
  for (unsigned i = 0, n = instance.getNumResults(); i < n; ++i) {
    if (i == modelPort)
      continue;
    unsigned newIndex = i + (i >= modelPort);
    if (instance.getPortNameStr(i) != expanded.getPortNameStr(newIndex) ||
        instance.getResult(i).getType() != expanded.getResult(newIndex).getType()) {
      error = "FAME output channel changed an unrelated instance port";
      return failure();
    }
    instance.getResult(i).replaceAllUsesWith(expanded.getResult(newIndex));
  }
  instance.erase();
  llvm::BitVector eraseInstance(expanded.getNumResults());
  eraseInstance.set(modelPort + 1);
  OpBuilder parentBuilder(expanded);
  InstanceOp replacement = expanded.erasePorts(parentBuilder, eraseInstance);
  expanded.erase();
  OpBuilder connectionBuilder(replacement);
  connectionBuilder.setInsertionPointAfter(replacement);
  connectionBuilder.create<ConnectOp>(replacement.getLoc(), newTop,
                                      replacement.getResult(modelPort));
  return success();
}
