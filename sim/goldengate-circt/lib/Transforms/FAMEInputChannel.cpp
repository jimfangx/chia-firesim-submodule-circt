// See LICENSE for license details.
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockGate.h"
#include "FAMEPortAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/HW/HWTypeInterfaces.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/SymbolTable.h"
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

LogicalResult rewriteMultiportInputChannel(
    const goldengate::TopHierarchy &hierarchy,
    const goldengate::FAMETopChannelPort &channel, std::string &error) {
  const auto &binding = *channel.binding;
  auto top = hierarchy.top;
  auto modelModule = binding.portGroup->module;
  auto model = dyn_cast<FModuleOp>(modelModule.getOperation());
  auto instance = binding.instance;
  auto channelType = channel.type;
  auto bitsIndex = channelType.getElementIndex("bits");
  auto payload = bitsIndex ? dyn_cast<BundleType>(
                                 channelType.getElements()[*bitsIndex].type)
                           : BundleType();
  if (!model || binding.portGroup->direction != Direction::In ||
      binding.instancePorts.size() < 2 ||
      instance->getParentOfType<FModuleOp>() != top || !payload ||
      payload.getElements().size() != binding.instancePorts.size()) {
    error = "expected a multiport bundle input on a top-level FAME model";
    return failure();
  }

  struct Leaf {
    unsigned modelPort;
    unsigned topPort;
    std::string field;
    Operation *connection;
  };
  llvm::SmallVector<Leaf> leaves;
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
      error = "FAME input bundle field does not match a model port";
      return failure();
    }
    std::optional<unsigned> topPort;
    for (const auto &connection : hierarchy.connections)
      if (connection.instance == instance &&
          connection.instancePort == modelPort) {
        if (topPort) {
          error = "FAME input bundle field has multiple top connections";
          return failure();
        }
        topPort = connection.topPort;
      }
    if (!topPort || !topPorts.insert(*topPort).second ||
        top.getPortDirection(*topPort) != Direction::In ||
        top.getPorts()[*topPort].type != model.getPorts()[modelPort].type ||
        removeCommonPrefix(top.getPortName(*topPort), binding.globalName) !=
            field) {
      error = "FAME input bundle field has no matching top port";
      return failure();
    }
    if (failed(goldengate::collectFAMEWrapperPayloadMetadata(
            top, *topPort, channel.type, field, wrapperAnnotations,
            wrapperSymbols, error)) ||
        failed(goldengate::collectFAMEPayloadSymbols(
            model, modelPort, channel.type, field, modelSymbols, error)))
      return failure();
    Value oldTop = top.getBodyBlock()->getArgument(*topPort);
    Value oldInstance = instance.getResult(modelPort);
    Operation *oldConnection = nullptr;
    for (OpOperand &use : oldInstance.getUses()) {
      Operation *connect = use.getOwner();
      auto strict = dyn_cast<StrictConnectOp>(connect);
      auto ordinary = dyn_cast<ConnectOp>(connect);
      if ((!strict && !ordinary) ||
          (strict && (strict.getDest() != oldInstance ||
                      strict.getSrc() != oldTop)) ||
          (ordinary && (ordinary.getDest() != oldInstance ||
                        ordinary.getSrc() != oldTop)) || oldConnection) {
        error = "FAME input bundle field has nontrivial top wiring";
        return failure();
      }
      oldConnection = connect;
    }
    if (!oldConnection || !oldTop.hasOneUse()) {
      error =
          "FAME input bundle field is not exclusively connected to its model";
      return failure();
    }
    leaves.push_back({modelPort, *topPort, field.str(), oldConnection});
  }

  std::string modelName = binding.portGroup->name + "_sink";
  for (const auto &port : model.getPorts())
    if (port.getName() == modelName) {
      error = "FAME model channel port already exists: " + modelName;
      return failure();
    }
  for (const auto &port : top.getPorts())
    if (port.getName() == channel.portName) {
      error = "FAME top channel port already exists: " + channel.portName;
      return failure();
    }

  unsigned modelInsert = *std::min_element(binding.instancePorts.begin(),
                                          binding.instancePorts.end());
  unsigned topInsert = *topPorts.begin();
  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelName), channel.type,
                     Direction::In);
  PortInfo topInfo(StringAttr::get(context, channel.portName), channel.type,
                   Direction::In);
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
        instance.getResult(i).getType() !=
            expanded.getResult(newIndex).getType()) {
      error = "FAME input bundle changed an unrelated instance port";
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
      replacement.getLoc(), replacement.getResult(modelInsert), newTop);
  return success();
}
} // namespace

LogicalResult goldengate::rewriteFAMEInputChannel(
    const TopHierarchy &hierarchy, const FAMETopChannelPort &channel,
    std::string &error) {
  const auto &binding = *channel.binding;
  auto top = hierarchy.top;
  auto modelModule = binding.portGroup->module;
  auto model = dyn_cast<FModuleOp>(modelModule.getOperation());
  auto instance = binding.instance;
  if (binding.instancePorts.size() > 1)
    return rewriteMultiportInputChannel(hierarchy, channel, error);
  if (!model || binding.portGroup->direction != Direction::In ||
      binding.instancePorts.size() != 1 ||
      instance->getParentOfType<FModuleOp>() != top) {
    error = "expected one scalar input on a top-level FAME model instance";
    return failure();
  }
  unsigned modelPort = binding.instancePorts.front();
  auto payloadType = dyn_cast<FIRRTLBaseType>(model.getPorts()[modelPort].type);
  if (!payloadType || isa<BundleType, FVectorType>(payloadType) ||
      !isa<BundleType>(channel.type) ||
      channel.type.getElements().size() != 3 ||
      channel.type.getElements()[2].type != payloadType) {
    error = "FAME input channel payload does not match the model port";
    return failure();
  }
  std::string modelName = binding.portGroup->name + "_sink";
  for (const auto &port : model.getPorts())
    if (port.getName() == modelName) {
      error = "FAME model channel port already exists: " + modelName;
      return failure();
    }
  for (const auto &port : top.getPorts())
    if (port.getName() == channel.portName) {
      error = "FAME top channel port already exists: " + channel.portName;
      return failure();
    }
  if (hasPortAnnotations(model, modelPort)) {
    error = "FAME input port has annotations requiring a field transfer";
    return failure();
  }

  std::optional<unsigned> topPort;
  for (const auto &connection : hierarchy.connections)
    if (connection.instance == instance &&
        connection.instancePort == modelPort) {
      if (topPort) {
        error = "FAME input channel has multiple top connections";
        return failure();
      }
      topPort = connection.topPort;
    }
  if (!topPort || top.getPortDirection(*topPort) != Direction::In ||
      top.getPorts()[*topPort].type != payloadType) {
    error = "FAME input channel has no matching top port";
    return failure();
  }
  llvm::SmallVector<Annotation> wrapperAnnotations;
  llvm::SmallVector<circt::hw::InnerSymPropertiesAttr> wrapperSymbols, modelSymbols;
  if (failed(collectFAMEWrapperPayloadMetadata(
          top, *topPort, channel.type, {}, wrapperAnnotations,
          wrapperSymbols, error)) ||
      failed(collectFAMEPayloadSymbols(
          model, modelPort, channel.type, {}, modelSymbols, error)))
    return failure();
  Value oldTop = top.getBodyBlock()->getArgument(*topPort);
  Value oldInstance = instance.getResult(modelPort);
  Operation *oldConnection = nullptr;
  for (OpOperand &use : oldInstance.getUses()) {
    auto *connect = use.getOwner();
    Value dest, src;
    if (auto strict = dyn_cast<StrictConnectOp>(connect)) {
      dest = strict.getDest();
      src = strict.getSrc();
    } else if (auto ordinary = dyn_cast<ConnectOp>(connect)) {
      dest = ordinary.getDest();
      src = ordinary.getSrc();
    }
    if (dest != oldInstance || src != oldTop || oldConnection) {
      error = "FAME input channel instance has nontrivial top wiring";
      return failure();
    }
    oldConnection = connect;
  }
  if (!oldConnection || !oldTop.hasOneUse()) {
    error = "FAME input channel top port is not exclusively connected to its model";
    return failure();
  }

  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelName), channel.type,
                     Direction::In);
  PortInfo topInfo(StringAttr::get(context, channel.portName), channel.type,
                   Direction::In);
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
  // The insertion position is in the original port list, as in the host
  // control rewrite. The old scalar moves one slot right and is then erased.
  model.insertPorts({{modelPort, modelInfo}});
  Value oldModel = model.getBodyBlock()->getArgument(modelPort + 1);
  OpBuilder body(model.getBodyBlock(), model.getBodyBlock()->begin());
  Value bits = body.create<SubfieldOp>(model.getLoc(),
                                       model.getBodyBlock()->getArgument(modelPort),
                                       "bits");
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
      error = "FAME input channel changed an unrelated instance port";
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
  connectionBuilder.create<ConnectOp>(replacement.getLoc(),
                                      replacement.getResult(modelPort), newTop);
  return success();
}

LogicalResult goldengate::addFAMEClockChannelToken(
    FModuleOp top, FModuleOp model, llvm::StringRef instanceName,
    llvm::StringRef topClockName, llvm::StringRef modelClockName,
    llvm::StringRef topChannelName, llvm::StringRef modelChannelName,
    BundleType channelType, std::string &error) {
  auto bitsIndex = channelType.getElementIndex("bits");
  auto readyIndex = channelType.getElementIndex("ready");
  auto validIndex = channelType.getElementIndex("valid");
  if (!bitsIndex || !readyIndex || !validIndex ||
      !isa<ClockType>(channelType.getElements()[*bitsIndex].type) ||
      !channelType.getElements()[*readyIndex].isFlip ||
      channelType.getElements()[*validIndex].isFlip) {
    error = "target clock channel has an invalid decoupled type";
    return failure();
  }

  std::optional<unsigned> topClock, modelClock;
  for (unsigned i = 0, n = top.getNumPorts(); i < n; ++i) {
    auto name = top.getPortName(i);
    if (name == topClockName)
      topClock = i;
    if (name == topChannelName) {
      error = "FAME top clock token port already exists";
      return failure();
    }
  }
  for (unsigned i = 0, n = model.getNumPorts(); i < n; ++i) {
    auto name = model.getPortName(i);
    if (name == modelClockName)
      modelClock = i;
    if (name == modelChannelName) {
      error = "FAME model clock token port already exists";
      return failure();
    }
  }
  if (!topClock || !modelClock ||
      top.getPortDirection(*topClock) != Direction::In ||
      model.getPortDirection(*modelClock) != Direction::In ||
      !isa<ClockType>(top.getPorts()[*topClock].type) ||
      !isa<ClockType>(model.getPorts()[*modelClock].type)) {
    error = "target clock token has no matching scalar clock ports";
    return failure();
  }

  InstanceOp instance;
  for (auto candidate : top.getOps<InstanceOp>())
    if (candidate.getName() == instanceName &&
        candidate.getModuleName() == model.getName()) {
      if (instance) {
        error = "target clock model instance is not unique";
        return failure();
      }
      instance = candidate;
    }
  if (!instance || instance.getNumResults() != model.getNumPorts() ||
      instance.getPortNameStr(*modelClock) != modelClockName) {
    error = "target clock model instance has incompatible ports";
    return failure();
  }
  bool clockConnected = false;
  Value oldTop = top.getBodyBlock()->getArgument(*topClock);
  Value oldInstance = instance.getResult(*modelClock);
  for (OpOperand &use : oldInstance.getUses()) {
    auto connection = dyn_cast<StrictConnectOp>(use.getOwner());
    if (!connection || connection.getDest() != oldInstance ||
        connection.getSrc() != oldTop || clockConnected) {
      error = "target clock instance has nontrivial top wiring";
      return failure();
    }
    clockConnected = true;
  }
  if (!clockConnected) {
    error = "target clock instance is not driven by the top clock";
    return failure();
  }

  auto *context = top.getContext();
  PortInfo modelInfo(StringAttr::get(context, modelChannelName), channelType,
                     Direction::In);
  PortInfo topInfo(StringAttr::get(context, topChannelName), channelType,
                   Direction::In);
  unsigned modelIndex = model.getNumPorts();
  unsigned topIndex = top.getNumPorts();
  model.insertPorts({{modelIndex, modelInfo}});
  top.insertPorts({{topIndex, topInfo}});
  InstanceOp replacement = instance.cloneAndInsertPorts({{modelIndex, modelInfo}});
  for (unsigned i = 0; i < modelIndex; ++i) {
    if (instance.getPortNameStr(i) != replacement.getPortNameStr(i) ||
        instance.getResult(i).getType() != replacement.getResult(i).getType()) {
      error = "target clock token changed an existing model port";
      return failure();
    }
    instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
  }
  instance.erase();
  OpBuilder connection(replacement);
  connection.setInsertionPointAfter(replacement);
  connection.create<ConnectOp>(replacement.getLoc(),
                               replacement.getResult(modelIndex),
                               top.getBodyBlock()->getArgument(topIndex));
  return success();
}

LogicalResult goldengate::addFAMEClockEnable(
    FModuleOp model, llvm::StringRef modelClockName, Value clockTokenBits,
    std::string &error) {
  Value hostClock, hostReset, finishing;
  circt::Namespace names;
  std::set<std::string> reserved;
  auto reserve = [&](StringRef name) {
    if (!name.empty() && reserved.insert(name.str()).second)
      names.newName(name);
  };
  for (unsigned i = 0, n = model.getNumPorts(); i < n; ++i) {
    reserve(model.getPortName(i));
    if (model.getPortDirection(i) != Direction::In)
      continue;
    if (model.getPortName(i) == "hostClock")
      hostClock = model.getBodyBlock()->getArgument(i);
    if (model.getPortName(i) == "hostReset")
      hostReset = model.getBodyBlock()->getArgument(i);
  }
  model.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) {
      reserve(name.getValue());
      if (name.getValue() == "targetCycleFinishing")
        if (auto wire = dyn_cast<WireOp>(op))
          finishing = wire.getResult();
    }
  });
  auto isBit = [](Value value) {
    auto type = value ? dyn_cast<UIntType>(value.getType()) : UIntType();
    return type && type.getWidth() == 1;
  };
  if (!hostClock || !isa<ClockType>(hostClock.getType()) ||
      !isBit(hostReset) || !isBit(finishing) ||
      (clockTokenBits && !isBit(clockTokenBits)) || modelClockName.empty()) {
    error = "target clock enable lacks a host clock, finishing signal, "
            "one-bit token (when present), or clock identity";
    return failure();
  }
  FAMEClockEnableIndex enables;
  if (failed(enables.collect(model, error)))
    return failure();
  if (enables.lookup(modelClockName, false)) {
    error = "target clock enable already exists for " + modelClockName.str();
    return failure();
  }
  // SFC hostFlagReg allocates once through the complete model namespace.
  std::string enableName = names.newName(modelClockName + "_enabled").str();
  auto *context = model.getContext();
  auto bitType = UIntType::get(context, 1, false);
  Location loc = model.getLoc();
  OpBuilder declarations(&model.getBodyBlock()->front());
  if (!clockTokenBits)
    clockTokenBits = declarations.create<ConstantOp>(
        loc, bitType, APInt(1, 1));
  Value resetZero = declarations.create<ConstantOp>(
      loc, bitType, APInt(1, 0));
  auto enableReg = declarations.create<RegResetOp>(
      loc, bitType, hostClock, hostReset, resetZero, enableName);
  enableReg->setAttr(fameClockEnableAttr,
                     declarations.getStringAttr(modelClockName));
  Value enabled = enableReg.getResult();
  OpBuilder body(context);
  body.setInsertionPointToEnd(model.getBodyBlock());
  Value next = body.create<MuxPrimOp>(loc, finishing, clockTokenBits, enabled);
  body.create<StrictConnectOp>(loc, enabled, next);
  return success();
}

LogicalResult goldengate::addFAMEClockGate(CircuitOp circuit, FModuleOp model,
                                           llvm::StringRef modelClockName,
                                           std::string &error,
                                           Value rawClockTokenBits) {
  if (model->getParentOp() != circuit.getOperation()) {
    error = "target clock model is outside the supplied circuit";
    return failure();
  }
  Value hostClock, hostReset, oldTargetClock, finishing, enabled;
  for (unsigned i = 0, n = model.getNumPorts(); i < n; ++i) {
    if (model.getPortDirection(i) != Direction::In)
      continue;
    Value port = model.getBodyBlock()->getArgument(i);
    if (model.getPortName(i) == "hostClock")
      hostClock = port;
    if (model.getPortName(i) == "hostReset")
      hostReset = port;
    if (model.getPortName(i) == modelClockName)
      oldTargetClock = port;
  }
  circt::Namespace names;
  std::set<std::string> reserved;
  for (auto port : model.getPorts())
    reserved.insert(port.getName().str());
  model.walk([&](Operation *op) {
    if (auto wire = dyn_cast<WireOp>(op);
        wire && wire.getName() == "targetCycleFinishing")
      finishing = wire.getResult();
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      reserved.insert(name.getValue().str());
  });
  for (const auto &name : reserved)
    names.newName(name);
  FAMEClockGateIndex gates;
  if (failed(gates.collect(model, error)))
    return failure();
  if (gates.lookup(modelClockName, false)) {
    error = "target clock gate already exists for " + modelClockName.str();
    return failure();
  }
  FAMEClockEnableIndex enables;
  if (failed(enables.collect(model, error)))
    return failure();
  enabled = enables.lookup(modelClockName);
  auto isBit = [](Value value) {
    auto type = value ? dyn_cast<UIntType>(value.getType()) : UIntType();
    return type && type.getWidth() == 1;
  };
  bool channelized = !oldTargetClock && rawClockTokenBits;
  if (!hostClock || (!oldTargetClock && !channelized) ||
      !isa<ClockType>(hostClock.getType()) ||
      (oldTargetClock && !isa<ClockType>(oldTargetClock.getType())) ||
      (channelized && !isa<ClockType>(rawClockTokenBits.getType())) ||
      !isBit(hostReset) ||
      !isBit(finishing) || !isBit(enabled) || modelClockName.empty()) {
    error = "target clock gate lacks the original/host clock, control "
            "signals, or clock identity";
    return failure();
  }

  // Validate channelized uses before creating the definition or instance.
  // The enable register keeps the raw token; all other clock reads are gated.
  SmallVector<SubfieldOp> clockReads;
  if (channelized) {
    auto rawBits = rawClockTokenBits.getDefiningOp<SubfieldOp>();
    auto input = rawBits ? dyn_cast<BlockArgument>(rawBits.getInput())
                         : BlockArgument();
    if (!rawBits || rawBits.getFieldName() != "bits" || !input ||
        input.getOwner() != model.getBodyBlock() ||
        model.getPortDirection(input.getArgNumber()) != Direction::In) {
      error = "target clock token is not a model input bits field";
      return failure();
    }
    for (OpOperand &use : rawBits.getInput().getUses()) {
      auto field = dyn_cast<SubfieldOp>(use.getOwner());
      if (field && field != rawBits && field.getFieldName() == "bits")
        clockReads.push_back(field);
    }
    if (clockReads.empty()) {
      error = "channelized target clock has no model uses to gate";
      return failure();
    }
  }
  auto enableReg = enabled.getDefiningOp<RegResetOp>();
  auto resetValue = enableReg.getResetValue().getDefiningOp<ConstantOp>();
  if (enableReg.getClockVal() != hostClock ||
      enableReg.getResetSignal() != hostReset || !resetValue ||
      !resetValue.getValue().isZero()) {
    error = "target clock enable requires the model host clock and reset to zero";
    return failure();
  }

  auto *context = circuit.getContext();
  Location loc = model.getLoc();
  auto clockType = ClockType::get(context);
  auto bitType = UIntType::get(context, 1, false);
  SmallVector<PortInfo> ports{
      PortInfo(StringAttr::get(context, "I"), clockType, Direction::In),
      PortInfo(StringAttr::get(context, "CE"), bitType, Direction::In),
      PortInfo(StringAttr::get(context, "O"), clockType, Direction::Out)};
  // DefineAbstractClockGate in the Scala oracle defines one shared blackbox;
  // FAMETransformer creates an independent buffer instance for each domain.
  // Resolve and check an existing definition before mutating the model.
  FExtModuleOp gate;
  if (auto *existing = SymbolTable::lookupSymbolIn(circuit, "AbstractClockGate")) {
    gate = dyn_cast<FExtModuleOp>(existing);
    if (!gate || gate.getDefname() != "AbstractClockGate" ||
        gate.getConvention() != Convention::Internal ||
        !gate.getParameters().empty() || !gate.getLayers().empty() ||
        gate.getNumPorts() != ports.size()) {
      error = "AbstractClockGate requires an unlayered external definition "
              "with internal convention, matching defname and no parameters";
      return failure();
    }
    for (auto [index, port] : llvm::enumerate(ports))
      if (gate.getPortName(index) != port.name.getValue() ||
          gate.getPortType(index) != port.type ||
          gate.getPortDirection(index) != port.direction) {
        error = "AbstractClockGate requires I:Clock, CE:UInt<1>, O:Clock";
        return failure();
      }
  } else {
    OpBuilder circuitBuilder(context);
    circuitBuilder.setInsertionPointToEnd(circuit.getBodyBlock());
    gate = circuitBuilder.create<FExtModuleOp>(
        loc, StringAttr::get(context, "AbstractClockGate"),
        ConventionAttr::get(context, Convention::Internal), ports,
        "AbstractClockGate");
  }

  OpBuilder declarations(&model.getBodyBlock()->front());
  // SFC allocates the buffer once in the complete model namespace.
  std::string bufferName = names.newName(modelClockName + "_buffer").str();
  auto buffer = declarations.create<InstanceOp>(loc, gate, bufferName);
  buffer->setAttr(fameClockGateAttr,
                  StringAttr::get(context, modelClockName));
  OpBuilder body(context);
  body.setInsertionPointToEnd(model.getBodyBlock());
  Value notReset = body.create<NotPrimOp>(loc, hostReset);
  Value enabledAndFinishing =
      body.create<AndPrimOp>(loc, enabled, finishing);
  Value gateEnable =
      body.create<AndPrimOp>(loc, enabledAndFinishing, notReset);
  body.create<StrictConnectOp>(loc, buffer.getResult(0), hostClock);
  body.create<StrictConnectOp>(loc, buffer.getResult(1), gateEnable);
  if (oldTargetClock) {
    oldTargetClock.replaceAllUsesWith(buffer.getResult(2));
  } else {
    for (auto field : clockReads)
      field.getResult().replaceAllUsesWith(buffer.getResult(2));
  }
  return success();
}

LogicalResult goldengate::removeFAMEVirtualClockPort(
    CircuitOp circuit, FModuleOp model, llvm::StringRef modelClockName,
    std::string &error) {
  std::optional<unsigned> clockPort;
  auto ports = model.getPorts();
  for (unsigned i = 0; i < ports.size(); ++i)
    if (ports[i].getName() == modelClockName)
      clockPort = i;
  if (model->getParentOp() != circuit.getOperation() || !clockPort ||
      ports[*clockPort].direction != Direction::In ||
      !isa<ClockType>(ports[*clockPort].type) ||
      hasPortAnnotations(model, *clockPort) || ports[*clockPort].sym ||
      !model.getBodyBlock()->getArgument(*clockPort).use_empty()) {
    error = "virtual target clock must be an unused, unannotated, "
            "unsymbolized scalar input in the supplied circuit";
    return failure();
  }

  SmallVector<InstanceOp> instances;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *node = graph.lookup(model);
    if (!node) {
      error = "virtual target clock model is absent from the instance graph";
      return failure();
    }
    for (auto *record : node->uses()) {
      auto instance = record->getInstance<InstanceOp>();
      if (!instance || instance.getNumResults() != ports.size()) {
        error = "virtual target clock instance has incompatible ports";
        return failure();
      }
      for (unsigned i = 0; i < ports.size(); ++i)
        if (instance.getPortNameStr(i) != ports[i].getName() ||
            instance.getResult(i).getType() != ports[i].type ||
            instance.getPortDirection(i) != ports[i].direction) {
          error = "virtual target clock instance has incompatible ports";
          return failure();
        }
      if (!cast<ArrayAttr>(instance.getPortAnnotationsAttr()[*clockPort]).empty()) {
        error = "virtual target clock instance port has annotations";
        return failure();
      }
      Value clock = instance.getResult(*clockPort);
      for (OpOperand &use : clock.getUses()) {
        auto strict = dyn_cast<StrictConnectOp>(use.getOwner());
        auto ordinary = dyn_cast<ConnectOp>(use.getOwner());
        // A clock read could feed other state. Only ancillary writes to the
        // consumed input may be dropped (including conditional writes).
        if (use.getOperandNumber() != 0 ||
            (!strict && !ordinary)) {
          error = "virtual target clock instance has non-write uses";
          return failure();
        }
      }
      instances.push_back(instance);
    }
  }

  // Every use has been checked before changing any model or instance.
  for (auto instance : instances) {
    Value clock = instance.getResult(*clockPort);
    for (OpOperand &use : llvm::make_early_inc_range(clock.getUses()))
      use.getOwner()->erase();
    llvm::BitVector removed(instance.getNumResults());
    removed.set(*clockPort);
    OpBuilder builder(instance);
    auto replacement = instance.erasePorts(builder, removed);
    // erasePorts rebuilds the declared FIRRTL attributes. Retain metadata
    // owned by other passes; only the per-port arrays change here.
    for (auto attr : instance->getAttrs())
      if (attr.getName() != "portNames" &&
          attr.getName() != "portDirections" &&
          attr.getName() != "portAnnotations")
        replacement->setAttr(attr.getName(), attr.getValue());
    for (unsigned i = 0; i < instance.getNumResults(); ++i)
      if (i != *clockPort)
        instance.getResult(i).replaceAllUsesWith(
            replacement.getResult(i - (i > *clockPort)));
    instance.erase();
  }
  llvm::BitVector removed(model.getNumPorts());
  removed.set(*clockPort);
  model.erasePorts(removed);
  return success();
}

LogicalResult goldengate::removeFAMETargetClockPort(
    FModuleOp top, FModuleOp model, llvm::StringRef instanceName,
    llvm::StringRef topClockName, llvm::StringRef modelClockName,
    std::string &error) {
  std::optional<unsigned> topClock, modelClock;
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    if (top.getPortName(i) == topClockName)
      topClock = i;
  for (unsigned i = 0; i < model.getNumPorts(); ++i)
    if (model.getPortName(i) == modelClockName)
      modelClock = i;
  if (!topClock || !modelClock ||
      top.getPortDirection(*topClock) != Direction::In ||
      model.getPortDirection(*modelClock) != Direction::In ||
      !isa<ClockType>(top.getPorts()[*topClock].type) ||
      !isa<ClockType>(model.getPorts()[*modelClock].type) ||
      hasPortAnnotations(top, *topClock) ||
      hasPortAnnotations(model, *modelClock) ||
      top.getPorts()[*topClock].sym || model.getPorts()[*modelClock].sym) {
    error = "target clock scalar ports are missing, incompatible, annotated, "
            "or symbolized";
    return failure();
  }
  Value topClockValue = top.getBodyBlock()->getArgument(*topClock);
  Value modelClockValue = model.getBodyBlock()->getArgument(*modelClock);
  if (!modelClockValue.use_empty()) {
    error = "target clock still has uses inside the model";
    return failure();
  }

  InstanceOp instance;
  for (auto candidate : top.getOps<InstanceOp>())
    if (candidate.getName() == instanceName &&
        candidate.getModuleName() == model.getName()) {
      if (instance) {
        error = "target clock model instance is not unique";
        return failure();
      }
      instance = candidate;
    }
  if (!instance || instance.getNumResults() != model.getNumPorts() ||
      instance.getPortNameStr(*modelClock) != modelClockName) {
    error = "target clock model instance has incompatible ports";
    return failure();
  }
  if (!cast<ArrayAttr>(instance.getPortAnnotationsAttr()[*modelClock]).empty()) {
    error = "target clock instance port has annotations without a deletion policy";
    return failure();
  }
  Value instanceClock = instance.getResult(*modelClock);
  StrictConnectOp connection;
  for (OpOperand &use : instanceClock.getUses()) {
    auto connect = dyn_cast<StrictConnectOp>(use.getOwner());
    if (!connect || connect.getDest() != instanceClock ||
        connect.getSrc() != topClockValue || connection) {
      error = "target clock model instance has nontrivial wiring";
      return failure();
    }
    connection = connect;
  }
  if (!connection || !topClockValue.hasOneUse()) {
    error = "target clock top port is not exclusively wired to the model";
    return failure();
  }

  connection.erase();
  llvm::BitVector eraseInstance(instance.getNumResults());
  eraseInstance.set(*modelClock);
  OpBuilder builder(instance);
  InstanceOp replacement = instance.erasePorts(builder, eraseInstance);
  for (auto attr : instance->getAttrs())
    if (attr.getName() != "portNames" &&
        attr.getName() != "portDirections" &&
        attr.getName() != "portAnnotations")
      replacement->setAttr(attr.getName(), attr.getValue());
  for (unsigned i = 0; i < instance.getNumResults(); ++i) {
    if (i == *modelClock)
      continue;
    unsigned replacementIndex = i - (i > *modelClock);
    if (instance.getPortNameStr(i) !=
            replacement.getPortNameStr(replacementIndex) ||
        instance.getResult(i).getType() !=
            replacement.getResult(replacementIndex).getType()) {
      error = "target clock removal changed an unrelated instance port";
      return failure();
    }
    instance.getResult(i).replaceAllUsesWith(
        replacement.getResult(replacementIndex));
  }
  instance.erase();
  llvm::BitVector eraseModel(model.getNumPorts());
  eraseModel.set(*modelClock);
  model.erasePorts(eraseModel);
  llvm::BitVector eraseTop(top.getNumPorts());
  eraseTop.set(*topClock);
  top.erasePorts(eraseTop);
  return success();
}

LogicalResult goldengate::internalizeFAMEOutputClocks(
    FModuleOp top, FModuleOp model, llvm::StringRef instanceName,
    std::string &error) {
  InstanceOp instance;
  for (auto candidate : top.getOps<InstanceOp>())
    if (candidate.getName() == instanceName &&
        candidate.getModuleName() == model.getName()) {
      if (instance) {
        error = "FAME model instance is not unique";
        return failure();
      }
      instance = candidate;
    }
  if (!instance || instance.getNumResults() != model.getNumPorts()) {
    error = "FAME model instance has incompatible ports";
    return failure();
  }

  struct ClockOutput {
    unsigned modelPort;
    unsigned topPort;
    StrictConnectOp connection;
    circt::hw::InnerSymAttr symbol;
  };
  SmallVector<ClockOutput> clocks;
  std::set<unsigned> topPorts;
  for (unsigned modelPort = 0; modelPort < model.getNumPorts(); ++modelPort) {
    if (model.getPortDirection(modelPort) != Direction::Out ||
        !isa<ClockType>(model.getPorts()[modelPort].type))
      continue;
    auto name = model.getPortName(modelPort);
    std::optional<unsigned> topPort;
    for (unsigned i = 0; i < top.getNumPorts(); ++i)
      if (top.getPortName(i) == name)
        topPort = i;
    if (!topPort || !topPorts.insert(*topPort).second ||
        top.getPortDirection(*topPort) != Direction::Out ||
        top.getPorts()[*topPort].type != model.getPorts()[modelPort].type ||
        instance.getPortNameStr(modelPort) != name ||
        hasPortAnnotations(top, *topPort) ||
        hasPortAnnotations(model, modelPort) || top.getPorts()[*topPort].sym ||
        !cast<ArrayAttr>(instance.getPortAnnotationsAttr()[modelPort]).empty()) {
      error = "FAME output clock metadata has no transfer policy: " +
              name.str();
      return failure();
    }
    auto symbol = model.getPorts()[modelPort].sym;
    if (symbol)
      for (auto property : symbol)
        if (property.getFieldID() != 0) {
          error = "FAME ground clock output symbol has a nonzero field ID";
          return failure();
        }
    for (auto &op : *model.getBodyBlock())
      if (auto localName = op.getAttrOfType<StringAttr>("name"))
        if (localName.getValue() == name) {
          error = "FAME output clock wire name collides with a declaration: " +
                  name.str();
          return failure();
        }
    Value topClock = top.getBodyBlock()->getArgument(*topPort);
    Value instanceClock = instance.getResult(modelPort);
    StrictConnectOp connection;
    for (OpOperand &use : instanceClock.getUses()) {
      auto connect = dyn_cast<StrictConnectOp>(use.getOwner());
      if (!connect || connect.getSrc() != instanceClock ||
          connect.getDest() != topClock || connection) {
        error = "FAME output clock has nontrivial top wiring: " + name.str();
        return failure();
      }
      connection = connect;
    }
    if (!connection || !topClock.hasOneUse()) {
      error = "FAME output clock is not exclusively wired to its top port: " +
              name.str();
      return failure();
    }
    clocks.push_back({modelPort, *topPort, connection, symbol});
  }
  if (clocks.empty())
    return success();

  // The model interface can only be changed here when this is its only use.
  // Refresh the instance graph after the rewrite, like port/symbol analyses.
  if (top->getParentOp() != model->getParentOp()) {
    error = "FAME output clock model is outside the wrapper circuit";
    return failure();
  }
  {
    circt::igraph::InstanceGraph graph(top->getParentOp());
    auto *node = graph.lookup(model);
    if (!node) {
      error = "FAME output clock model is absent from the instance graph";
      return failure();
    }
    unsigned uses = 0;
    for (auto *record : node->uses()) {
      if (record->getInstance<InstanceOp>() != instance) {
        error = "FAME output clock model has another instance";
        return failure();
      }
      ++uses;
    }
    if (uses != 1) {
      error = "FAME output clock model instance is absent from the graph";
      return failure();
    }
  }

  // SFC's unusedOutputsAsWires preserves model-body connects to these
  // former clock ports. The MLIR block argument has the same role until the
  // replacement wire takes over all its uses.
  OpBuilder declarations(model.getBodyBlock(), model.getBodyBlock()->begin());
  llvm::BitVector eraseModel(model.getNumPorts());
  llvm::BitVector eraseTop(top.getNumPorts());
  llvm::BitVector eraseInstance(instance.getNumResults());
  for (auto &clock : clocks) {
    auto name = model.getPortName(clock.modelPort);
    auto wire = declarations.create<WireOp>(
        model.getLoc(), model.getPorts()[clock.modelPort].type, name);
    if (clock.symbol)
      wire.setInnerSymAttr(clock.symbol);
    model.getBodyBlock()->getArgument(clock.modelPort)
        .replaceAllUsesWith(wire.getResult());
    clock.connection.erase();
    eraseModel.set(clock.modelPort);
    eraseTop.set(clock.topPort);
    eraseInstance.set(clock.modelPort);
  }

  OpBuilder builder(instance);
  InstanceOp replacement = instance.erasePorts(builder, eraseInstance);
  for (auto attr : instance->getAttrs())
    if (attr.getName() != "portNames" &&
        attr.getName() != "portDirections" &&
        attr.getName() != "portAnnotations")
      replacement->setAttr(attr.getName(), attr.getValue());
  unsigned replacementPort = 0;
  for (unsigned oldPort = 0; oldPort < instance.getNumResults(); ++oldPort) {
    if (eraseInstance.test(oldPort))
      continue;
    instance.getResult(oldPort).replaceAllUsesWith(
        replacement.getResult(replacementPort++));
  }
  instance.erase();
  model.erasePorts(eraseModel);
  top.erasePorts(eraseTop);
  return success();
}

LogicalResult goldengate::internalizeFAMEUnusedOutputs(
    CircuitOp circuit, FModuleOp model, ArrayRef<StringRef> portNames,
    std::string &error) {
  if (portNames.empty())
    return success();
  if (model->getParentOp() != circuit.getOperation()) {
    error = "unused output model is outside the supplied circuit";
    return failure();
  }
  auto ports = model.getPorts();
  llvm::BitVector removed(ports.size());
  for (StringRef name : portNames) {
    std::optional<unsigned> index;
    for (unsigned i = 0; i < ports.size(); ++i)
      if (ports[i].getName() == name)
        index = i;
    auto type = index ? dyn_cast<FIRRTLBaseType>(ports[*index].type)
                      : FIRRTLBaseType();
    if (name.empty() || !index || removed.test(*index) ||
        ports[*index].direction != Direction::Out || !type ||
        !type.isPassive() || isa<ClockType>(type) ||
        hasPortAnnotations(model, *index)) {
      error = "unused output must be a unique, passive, unannotated "
              "data output: " + name.str();
      return failure();
    }
    // The wire has the identical type, so ground identities keep their field
    // IDs. CIRCT LowerTypes cannot preserve symbols targeting aggregates.
    // Validate both the range and target type before changing any IR.
    auto fields = dyn_cast<circt::hw::FieldIDTypeInterface>(type);
    uint64_t maxFieldID = fields ? fields.getMaxFieldID() : 0;
    if (auto symbol = ports[*index].sym)
      for (auto property : symbol) {
        if (property.getFieldID() > maxFieldID) {
          error = "unused output inner symbol has an invalid field ID: " +
                  name.str();
          return failure();
        }
        auto targetType = circt::hw::FieldIdImpl::getFinalTypeByFieldID(
            type, property.getFieldID());
        if (isa<BundleType, FVectorType>(targetType)) {
          error = "unused output inner symbol targets an aggregate unsupported "
                  "by LowerTypes: " + name.str();
          return failure();
        }
      }
    // Replacing a port with a wire must preserve its local target identity.
    for (auto &op : *model.getBodyBlock())
      if (auto localName = op.getAttrOfType<StringAttr>("name"))
        if (localName.getValue() == name) {
          error = "unused output wire name collides with a declaration: " +
                  name.str();
          return failure();
        }
    removed.set(*index);
  }

  SmallVector<InstanceOp> instances;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *node = graph.lookup(model);
    if (!node) {
      error = "unused output model is absent from the instance graph";
      return failure();
    }
    for (auto *record : node->uses()) {
      auto instance = record->getInstance<InstanceOp>();
      if (!instance || instance.getNumResults() != ports.size()) {
        error = "unused output instance has incompatible ports";
        return failure();
      }
      for (unsigned i = 0; i < ports.size(); ++i) {
        if (instance.getPortNameStr(i) != ports[i].getName() ||
            instance.getResult(i).getType() != ports[i].type ||
            instance.getPortDirection(i) != ports[i].direction) {
          error = "unused output instance has incompatible ports";
          return failure();
        }
        if (removed.test(i) &&
            (!instance.getResult(i).use_empty() ||
             !cast<ArrayAttr>(instance.getPortAnnotationsAttr()[i]).empty())) {
          error = "unused output instance result is still used or annotated: " +
                  ports[i].getName().str();
          return failure();
        }
      }
      instances.push_back(instance);
    }
  }

  // SFC's unusedOutputsAsWires keeps body connects legal after passthrough
  // promotion routes their former consumers directly from upstream sources.
  OpBuilder declarations(model.getBodyBlock(), model.getBodyBlock()->begin());
  for (unsigned i = 0; i < ports.size(); ++i)
    if (removed.test(i)) {
      auto wire = declarations.create<WireOp>(
          ports[i].loc, ports[i].type, ports[i].getName());
      if (ports[i].sym)
        wire.setInnerSymAttr(ports[i].sym);
      model.getBodyBlock()->getArgument(i).replaceAllUsesWith(wire.getResult());
    }
  for (auto instance : instances) {
    OpBuilder builder(instance);
    auto replacement = instance.erasePorts(builder, removed);
    for (auto attr : instance->getAttrs())
      if (attr.getName() != "portNames" &&
          attr.getName() != "portDirections" &&
          attr.getName() != "portAnnotations")
        replacement->setAttr(attr.getName(), attr.getValue());
    unsigned next = 0;
    for (unsigned i = 0; i < ports.size(); ++i)
      if (!removed.test(i))
        instance.getResult(i).replaceAllUsesWith(replacement.getResult(next++));
    instance.erase();
  }
  model.erasePorts(removed);
  return success();
}

LogicalResult goldengate::groupFAMEChannelPorts(
    FModuleOp top, FModuleOp model, llvm::StringRef instanceName,
    llvm::StringRef modelClockSink, std::string &error) {
  InstanceOp instance;
  for (auto candidate : top.getOps<InstanceOp>())
    if (candidate.getName() == instanceName &&
        candidate.getModuleName() == model.getName()) {
      if (instance) {
        error = "FAME model instance is not unique";
        return failure();
      }
      instance = candidate;
    }
  if (!instance || instance.getNumResults() != model.getNumPorts()) {
    error = "FAME model instance has incompatible ports";
    return failure();
  }
  for (unsigned i = 0; i < model.getNumPorts(); ++i)
    if (instance.getPortNameStr(i) != model.getPortName(i) ||
        instance.getResult(i).getType() != model.getPorts()[i].type) {
      error = "FAME model instance port differs from its module";
      return failure();
    }

  auto planOrder = [&](FModuleOp module, bool modelPorts,
                       SmallVectorImpl<unsigned> &order) -> LogicalResult {
    std::set<unsigned> added;
    auto addNamedInput = [&](llvm::StringRef name) -> bool {
      for (unsigned i = 0; i < module.getNumPorts(); ++i)
        if (module.getPortName(i) == name &&
            module.getPortDirection(i) == Direction::In) {
          order.push_back(i);
          added.insert(i);
          return true;
        }
      return false;
    };
    if (!addNamedInput("hostClock") || !addNamedInput("hostReset") ||
        (modelPorts && !addNamedInput(modelClockSink))) {
      error = "FAME module is missing a host control or clock sink port";
      return failure();
    }
    for (Direction direction : {Direction::In, Direction::Out})
      for (unsigned i = 0; i < module.getNumPorts(); ++i)
        if (module.getPortDirection(i) == direction && !added.count(i)) {
          order.push_back(i);
          added.insert(i);
        }
    if (order.size() != module.getNumPorts()) {
      error = "FAME module has an unsupported port direction";
      return failure();
    }
    return success();
  };
  SmallVector<unsigned> modelOrder, topOrder;
  if (failed(planOrder(model, true, modelOrder)) ||
      failed(planOrder(top, false, topOrder)))
    return failure();

  // CIRCT's insert/erase APIs update the port metadata and block arguments
  // together. Insert the desired permutation, redirect SSA uses from the old
  // arguments/results, then erase the old half of each port list.
  auto reorderModule = [](FModuleOp module, ArrayRef<unsigned> order) {
    unsigned oldSize = module.getNumPorts();
    SmallVector<std::pair<unsigned, PortInfo>> added;
    auto oldPorts = module.getPorts();
    for (unsigned index : order)
      added.emplace_back(0, oldPorts[index]);
    module.insertPorts(added);
    for (unsigned newIndex = 0; newIndex < oldSize; ++newIndex)
      module.getBodyBlock()->getArgument(oldSize + order[newIndex])
          .replaceAllUsesWith(module.getBodyBlock()->getArgument(newIndex));
    llvm::BitVector oldIndices(module.getNumPorts());
    oldIndices.set(oldSize, module.getNumPorts());
    module.erasePorts(oldIndices);
    return added;
  };
  bool modelChanged = false;
  for (unsigned i = 0; i < modelOrder.size(); ++i)
    modelChanged |= modelOrder[i] != i;
  if (modelChanged) {
    unsigned oldSize = model.getNumPorts();
    auto added = reorderModule(model, modelOrder);
    InstanceOp expanded = instance.cloneAndInsertPorts(added);
    for (unsigned newIndex = 0; newIndex < oldSize; ++newIndex)
      instance.getResult(modelOrder[newIndex])
          .replaceAllUsesWith(expanded.getResult(newIndex));
    instance.erase();
    llvm::BitVector oldIndices(expanded.getNumResults());
    oldIndices.set(oldSize, expanded.getNumResults());
    OpBuilder builder(expanded);
    expanded.erasePorts(builder, oldIndices);
    expanded.erase();
  }
  bool topChanged = false;
  for (unsigned i = 0; i < topOrder.size(); ++i)
    topChanged |= topOrder[i] != i;
  if (topChanged)
    reorderModule(top, topOrder);
  return success();
}

LogicalResult goldengate::removeFAMEAncillaryTopClockConnects(
    FModuleOp top, InstanceOp modelInstance, std::string &error) {
  if (!modelInstance || modelInstance->getBlock() != top.getBodyBlock()) {
    error = "ancillary clock removal requires a direct wrapper model instance";
    return failure();
  }
  SmallVector<Operation *> connections;
  top.walk([&](Operation *op) {
    Value dest, src;
    if (auto connect = dyn_cast<StrictConnectOp>(op)) {
      dest = connect.getDest();
      src = connect.getSrc();
    } else if (auto connect = dyn_cast<ConnectOp>(op)) {
      dest = connect.getDest();
      src = connect.getSrc();
    }
    if (!dest || !isa<ClockType>(dest.getType()))
      return;
    auto isWrapperPort = [&](Value value) {
      auto arg = dyn_cast<BlockArgument>(value);
      return arg && arg.getOwner() == top.getBodyBlock();
    };
    // These original direct port connects are required by the staged scalar
    // clock removal helpers. They are erased there, before the final boundary.
    if ((dest.getDefiningOp() == modelInstance && isWrapperPort(src)) ||
        (src.getDefiningOp() == modelInstance && isWrapperPort(dest)))
      return;
    connections.push_back(op);
  });
  for (auto *connect : connections)
    connect->erase();
  return success();
}

LogicalResult goldengate::removeFAMEStaleTopClocks(FModuleOp top,
                                                std::string &error) {
  auto ports = top.getPorts();
  llvm::BitVector removed(ports.size());
  bool hostClockFound = false;
  std::set<Operation *> connections;
  for (unsigned i = 0; i < ports.size(); ++i) {
    if (ports[i].getName() == "hostClock") {
      hostClockFound = isa<ClockType>(ports[i].type) &&
                       ports[i].direction == Direction::In;
      continue;
    }
    if (!isa<ClockType>(ports[i].type))
      continue;
    if (hasPortAnnotations(top, i) || ports[i].sym) {
      error = "stale top clock has annotations or an inner symbol: " +
              ports[i].getName().str();
      return failure();
    }
    for (auto *user : top.getBodyBlock()->getArgument(i).getUsers()) {
      Value dest;
      if (auto connect = dyn_cast<StrictConnectOp>(user))
        dest = connect.getDest();
      else if (auto connect = dyn_cast<ConnectOp>(user))
        dest = connect.getDest();
      if (!dest || !isa<ClockType>(dest.getType())) {
        error = "stale top clock has a non-clock-connect use: " +
                ports[i].getName().str();
        return failure();
      }
      connections.insert(user);
    }
    removed.set(i);
  }
  if (!hostClockFound) {
    error = "FAME top has no scalar hostClock input";
    return failure();
  }
  if (removed.none())
    return success();
  auto circuit = top->getParentOfType<CircuitOp>();
  if (!circuit) {
    error = "FAME top is outside a circuit";
    return failure();
  }
  circt::igraph::InstanceGraph graph(circuit);
  auto *node = graph.lookup(top);
  if (!node || !node->uses().empty()) {
    error = "stale clock removal requires an uninstantiated FAME top";
    return failure();
  }
  // SFC transformTop filters Clock ports, while updateNonChannelConnects
  // drops their ancillary clock assignments. Deduplicate a connect whose
  // source and destination are both stale ports before erasing it.
  for (auto *connect : connections)
    connect->erase();
  top.erasePorts(removed);
  return success();
}

LogicalResult goldengate::orderFAMETopPorts(
    FModuleOp top, ArrayRef<llvm::StringRef> retainedPortNames,
    ArrayRef<llvm::StringRef> channelPortNames, std::string &error) {
  SmallVector<unsigned> order;
  std::set<unsigned> used;
  for (llvm::StringRef name : {"hostClock", "hostReset"}) {
    if (!llvm::is_contained(retainedPortNames, name)) {
      error = "FAME retained top ports are missing host port " + name.str();
      return failure();
    }
  }
  auto appendPorts = [&](ArrayRef<llvm::StringRef> names,
                         StringRef kind) -> LogicalResult {
    for (llvm::StringRef name : names) {
      bool found = false;
      for (unsigned i = 0; i < top.getNumPorts(); ++i)
        if (!name.empty() && top.getPortName(i) == name) {
          if (!used.insert(i).second) {
            error = "FAME top " + kind.str() + " port is duplicated: " +
                    name.str();
            return failure();
          }
          order.push_back(i);
          found = true;
          break;
        }
      if (!found) {
        error = "FAME top " + kind.str() + " port is missing: " + name.str();
        return failure();
      }
    }
    return success();
  };
  if (failed(appendPorts(retainedPortNames, "retained")) ||
      failed(appendPorts(channelPortNames, "channel")))
    return failure();
  if (order.size() != top.getNumPorts()) {
    error = "FAME top has ports outside the retained/channel plan";
    return failure();
  }
  if (llvm::all_of(llvm::enumerate(order),
                   [](auto entry) { return entry.index() == entry.value(); }))
    return success();

  unsigned count = top.getNumPorts();
  auto oldPorts = top.getPorts();
  SmallVector<std::pair<unsigned, PortInfo>> added;
  for (unsigned index : order)
    added.emplace_back(0, oldPorts[index]);
  top.insertPorts(added);
  for (unsigned i = 0; i < count; ++i)
    top.getBodyBlock()->getArgument(count + order[i])
        .replaceAllUsesWith(top.getBodyBlock()->getArgument(i));
  llvm::BitVector oldIndices(top.getNumPorts());
  oldIndices.set(count, top.getNumPorts());
  top.erasePorts(oldIndices);
  return success();
}
