// See LICENSE for license details.
#include "goldengate/FAMEOutputChannel.h"
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
    unsigned topPort;
    std::string field;
    Operation *connection;
  };
  llvm::SmallVector<Leaf> leaves;
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
    std::optional<unsigned> topPort;
    for (const auto &connection : hierarchy.connections)
      if (connection.instance == instance &&
          connection.instancePort == modelPort) {
        if (topPort) {
          error = "FAME output bundle field has multiple top connections";
          return failure();
        }
        topPort = connection.topPort;
      }
    if (!topPort || !topPorts.insert(*topPort).second ||
        top.getPortDirection(*topPort) != Direction::Out ||
        top.getPorts()[*topPort].type != model.getPorts()[modelPort].type ||
        removeCommonPrefix(top.getPortName(*topPort), binding.globalName) !=
            field ||
        hasPortAnnotations(top, *topPort)) {
      error = "FAME output bundle field has no matching unannotated top port";
      return failure();
    }
    Value oldTop = top.getBodyBlock()->getArgument(*topPort);
    Value oldInstance = instance.getResult(modelPort);
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
    if (!oldConnection || !oldInstance.hasOneUse()) {
      error = "FAME output bundle field is not exclusively connected to its top port";
      return failure();
    }
    leaves.push_back({modelPort, *topPort, field.str(), oldConnection});
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
  model.insertPorts({{modelInsert, modelInfo}});
  OpBuilder body(&model.getBodyBlock()->front());
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
  for (auto &leaf : leaves)
    leaf.connection->erase();
  llvm::BitVector eraseTop(top.getNumPorts());
  for (const auto &leaf : leaves)
    eraseTop.set(leaf.topPort + 1);
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
  for (const auto &port : top.getPorts())
    if (port.getName() == channel.portName) {
      error = "FAME top source port already exists";
      return failure();
    }

  std::optional<unsigned> topPort;
  for (const auto &connection : hierarchy.connections)
    if (connection.instance == instance &&
        connection.instancePort == modelPort) {
      if (topPort) {
        error = "FAME output channel has multiple top connections";
        return failure();
      }
      topPort = connection.topPort;
    }
  if (!topPort || top.getPortDirection(*topPort) != Direction::Out ||
      top.getPorts()[*topPort].type != payloadType ||
      hasPortAnnotations(top, *topPort)) {
    error = "FAME output channel has no unannotated matching top port";
    return failure();
  }
  Value oldTop = top.getBodyBlock()->getArgument(*topPort);
  Value oldInstance = instance.getResult(modelPort);
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
  if (!oldConnection || !oldInstance.hasOneUse()) {
    error = "FAME output channel is not exclusively connected to its top port";
    return failure();
  }

  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelName), channel.type,
                     Direction::Out);
  PortInfo topInfo(StringAttr::get(context, channel.portName), channel.type,
                   Direction::Out);
  model.insertPorts({{modelPort, modelInfo}});
  Value oldModel = model.getBodyBlock()->getArgument(modelPort + 1);
  OpBuilder body(&model.getBodyBlock()->front());
  Value bits = body.create<SubfieldOp>(model.getLoc(),
      model.getBodyBlock()->getArgument(modelPort), "bits");
  oldModel.replaceAllUsesWith(bits);
  llvm::BitVector eraseModel(model.getNumPorts());
  eraseModel.set(modelPort + 1);
  model.erasePorts(eraseModel);

  top.insertPorts({{*topPort, topInfo}});
  Value newTop = top.getBodyBlock()->getArgument(*topPort);
  oldConnection->erase();
  llvm::BitVector eraseTop(top.getNumPorts());
  eraseTop.set(*topPort + 1);
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
