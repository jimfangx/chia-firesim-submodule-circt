// See LICENSE for license details.
#include "goldengate/SRAMModelChannels.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/ChannelExcision.h"
#include "goldengate/ExtractModel.h"
#include "goldengate/FAMEDefaults.h"
#include "goldengate/FAMEHostControl.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/FAMEFiredState.h"
#include "goldengate/FAMEInputReady.h"
#include "goldengate/FAMEOutputValid.h"
#include "goldengate/FAMEFinishing.h"
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/InferModelPorts.h"
#include "goldengate/LabelSRAMModels.h"
#include "goldengate/LowerTypes.h"
#include "goldengate/PromotePassthroughConnections.h"
#include "goldengate/RemainingFanout.h"
#include "goldengate/RAMModelAdapter.h"
#include "goldengate/XDCEmission.h"
#include "FAMEPortAnnotations.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "llvm/ADT/BitVector.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/PassManager.h"
#include <functional>
#include <map>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

/// Required input invariants: wrapped top, resolved last-connect semantics,
/// retained memory selections and a target-clock channel on the hub input.
/// Annotations consumed: FirrtlMemModelAnnotation, FirrtlFAMEModelAnnotation.
/// Annotations produced: SRAM port kinds, FAME model/channel/port annotations,
/// DontTouch annotations. IR mutations: wrap/promote selected memories, lower
/// aggregate ports, promote passthroughs, and excise inter-model connections.
/// Analyses required: native hierarchy and channel target resolution.
/// Analyses preserved: none (ports and hierarchy change).
/// Output invariants: verified ground FIRRTL, clocked inter-model channels,
/// module-local channel groups ready for FAME analysis. No RAM timing model or
/// FAME hardware rewrite is performed by this boundary.
LogicalResult goldengate::prepareSRAMModelChannels(
    ModuleOp root, CircuitOp circuit, unsigned &wrapped, unsigned &promoted,
    std::string &error) {
  wrapped = promoted = 0;
  unsigned passthroughs = 0;
  PassManager normalization(root.getContext());
  normalization.nest<CircuitOp>().addNestedPass<FModuleOp>(createLowerCHIRRTLPass());
  normalization.addNestedPass<CircuitOp>(createInferWidthsPass());
  normalization.addNestedPass<CircuitOp>(createInferResetsPass());
  if (failed(normalization.run(root))) {
    error = "SRAM normalization failed";
    return failure();
  }
  auto stage = [&](StringRef name, auto run) {
    if (failed(run())) {
      error = name.str() + ": " + error;
      return failure();
    }
    return success();
  };
  if (failed(stage("LabelSRAMModels", [&] {
        return labelSRAMModels(circuit, wrapped, error);
      })) ||
      failed(stage("ExtractModel", [&] {
        return extractModels(circuit, promoted, error);
      })) ||
      failed(stage("LowerTypes", [&] {
        return lowerTypesWithRetainedTargets(root, circuit, error);
      })) ||
      failed(stage("PromotePassthroughConnections", [&] {
        return promotePassthroughConnections(circuit, passthroughs, error);
      })) ||
      failed(stage("FAMEDefaults", [&] { return addFAMEDefaults(circuit, error); })) ||
      failed(stage("FindDefaultClocks", [&] { return findDefaultClocks(circuit, error); })) ||
      failed(stage("ChannelExcision", [&] { return exciseChannels(circuit, error); })) ||
      failed(stage("InferModelPorts", [&] { return inferModelPorts(circuit, error); })))
    return failure();
  if (failed(verify(root))) {
    error = "SRAM channel preparation produced invalid FIRRTL IR";
    return failure();
  }
  return success();
}

std::optional<llvm::SmallVector<goldengate::SRAMModelDependencies>>
goldengate::analyzeSRAMModelDependencies(CircuitOp circuit, std::string &error) {
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!hierarchy || !raw) {
    if (error.empty()) error = "SRAM dependency analysis needs retained annotations";
    return std::nullopt;
  }
  SmallVector<ModelPortGroup> groups;
  SmallVector<FModuleOp> models;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(AnnotationClasses::ChannelPorts)) {
      auto group = analyzeModelPortGroup(circuit, annotation, error);
      if (!group) return std::nullopt;
      groups.push_back(std::move(*group));
    } else if (annotation.isClass(AnnotationClasses::FAMETransform)) {
      auto spelling = annotation.getMember<StringAttr>("target");
      auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error)
                             : std::nullopt;
      auto model = target ? dyn_cast<FModuleOp>(target->module.getOperation()) : FModuleOp();
      if (!model || target->port) {
        if (error.empty()) error = "SRAM FAME target is not an internal module";
        return std::nullopt;
      }
      if (!llvm::is_contained(models, model)) models.push_back(model);
    }
  }
  SmallVector<ModelChannelBinding> bindings;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection)) continue;
    auto channel = analyzeChannelConnection(circuit, annotation, error);
    if (!channel) return std::nullopt;
    auto bound = bindChannelToModels(*channel, *hierarchy, groups, error);
    if (!bound) return std::nullopt;
    bindings.append(bound->begin(), bound->end());
  }
  SmallVector<SRAMModelDependencies> result;
  for (auto model : models) {
    auto outputs = analyzeLocalChannelDependencies(model, bindings);
    for (const auto &output : outputs) {
      if (!output.unresolvedPorts.empty() || !output.unresolvedCauses.empty()) {
        error = "SRAM model " + model.getName().str() + " output " +
                output.outputChannel + " has unresolved dependencies";
        for (const auto &cause : output.unresolvedCauses) error += ": " + cause;
        return std::nullopt;
      }
    }
    result.push_back({model, std::move(outputs)});
  }
  return result;
}

/// This is a partial FAME hardware boundary, following InferModelPorts.
/// SRAM port annotations select definitions, rather than instance spellings.
/// Virtual clock valid/enable are constant one, but the buffered enable starts
/// at zero and changes only when the target cycle finishes. All memory clock
/// reads are driven by the abstract gate; the original clock input and its
/// ancillary instance writes are removed. Data annotations/ports are preserved.
/// The finishing wire is deliberately left for the subsequent channel rewrite.
/// Hierarchy and port analyses must be rebuilt after this mutation.
LogicalResult goldengate::rewriteSRAMVirtualClocks(
    CircuitOp circuit, unsigned &rewritten, std::string &error) {
  rewritten = 0;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "SRAM virtual clocks need retained model/channel annotations";
    return failure();
  }
  SmallVector<FModuleOp> models;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ModelReadPort) &&
        !annotation.isClass(AnnotationClasses::ModelWritePort) &&
        !annotation.isClass(AnnotationClasses::ModelReadWritePort))
      continue;
    auto addr = annotation.getMember<StringAttr>("addr");
    auto target = addr ? resolveAnnotationTarget(circuit, addr.getValue(), error)
                       : std::nullopt;
    auto model = target ? dyn_cast<FModuleOp>(target->module.getOperation())
                        : FModuleOp();
    if (!model || !target->port) {
      error = "SRAM port annotation has no internal model address port";
      return failure();
    }
    if (!llvm::is_contained(models, model)) models.push_back(model);
  }
  if (models.empty()) {
    error = "SRAM virtual clocks need at least one prepared SRAM model";
    return failure();
  }
  // Check virtual-clock eligibility before adding controls to any definition.
  SmallVector<std::string> clocks;
  for (auto model : models) {
    std::string clock;
    unsigned clockCount = 0, groups = 0;
    for (auto port : model.getPorts())
      if (isa<ClockType>(port.type)) {
        if (port.direction != Direction::In) {
          error = "SRAM virtual clock must be an input";
          return failure();
        }
        clock = port.getName().str();
        ++clockCount;
      }
    if (clockCount != 1 || model.getOps<MemOp>().empty()) {
      error = "SRAM virtual clock needs one target clock and a memory in " +
              model.getName().str();
      return failure();
    }
    for (Attribute attr : raw) {
      Annotation annotation(attr);
      if (!annotation.isClass(AnnotationClasses::ChannelPorts)) continue;
      auto group = analyzeModelPortGroup(circuit, annotation, error);
      if (!group) return failure();
      if (group->module != model) continue;
      ++groups;
      if (group->clockPort || llvm::any_of(group->ports, [&](unsigned port) {
            return isa<ClockType>(model.getPorts()[port].type);
          })) {
        error = "SRAM " + model.getName().str() +
                " has an explicit local clock channel/association";
        return failure();
      }
    }
    if (!groups) {
      error = "SRAM virtual clock requires inferred data channel groups";
      return failure();
    }
    // Raw annotations are separate from CIRCT port metadata. A removed clock
    // must not leave a retained target behind (including unknown annotations).
    // Data targets are unchanged by this substep and keep their annotations.
    std::string clockTarget = "~" + circuit.getName().str() + "|" +
                              model.getName().str() + ">" + clock;
    std::function<bool(Attribute)> referencesClock = [&](Attribute attr) {
      if (auto spelling = dyn_cast<StringAttr>(attr))
        return spelling.getValue() == clockTarget;
      if (auto array = dyn_cast<ArrayAttr>(attr))
        return llvm::any_of(array, referencesClock);
      if (auto dictionary = dyn_cast<DictionaryAttr>(attr))
        return llvm::any_of(dictionary, [&](NamedAttribute member) {
          return referencesClock(member.getValue());
        });
      return false;
    };
    if (referencesClock(raw)) {
      error = "SRAM virtual clock has a retained annotation target: " + clockTarget;
      return failure();
    }
    clocks.push_back(std::move(clock));
  }
  for (auto pair : llvm::zip(models, clocks)) {
    auto model = std::get<0>(pair);
    const auto &clock = std::get<1>(pair);
    if (failed(addFAMEHostControl(circuit, model, error)) ||
        failed(addFAMEClockEnable(model, clock, Value(), error)) ||
        failed(addFAMEClockGate(circuit, model, clock, error)) ||
        failed(removeFAMEVirtualClockPort(circuit, model, clock, error)))
      return failure();
    ++rewritten;
  }
  return success();
}

namespace {
// Stable names and types survive the insert/erase APIs; indices and instance
// operations do not. Never reuse a binding after a channel has been rewritten.
struct SRAMDataPort {
  std::string model, instance, local, global, topPort, clock;
  SmallVector<std::string> oldPorts;
  Direction direction;
  BundleType type;
};

// SFC transformTop promotes bridge-to-bridge scalar passthroughs to whole
// Decoupled connects. Capture SSA endpoints before model rewrites invalidate
// port indices; these endpoints have no model binding of their own.
struct SRAMTopPassthrough {
  std::string input, output;
  PortInfo sink, source;
  Operation *connect;
};

std::optional<SmallVector<SRAMTopPassthrough>> analyzeSRAMTopPassthroughs(
    const goldengate::TopHierarchy &hierarchy,
    ArrayRef<goldengate::GGChannelConnection> channels,
    std::map<std::string, std::string> &renames, std::string &error) {
  using namespace goldengate;
  auto top = hierarchy.top;
  auto *context = top.getContext();
  SmallVector<SRAMTopPassthrough> result;
  std::set<unsigned> claimed;
  std::set<std::string> names;
  for (auto p : top.getPorts()) names.insert(p.getName().str());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name"))
      names.insert(name.getValue().str());
  });
  auto endpointChannel = [&](unsigned port, bool source) -> const GGChannelConnection * {
    const GGChannelConnection *found = nullptr;
    for (const auto &channel : channels) {
      auto &endpoints = source ? channel.sources : channel.sinks;
      for (const auto &endpoint : endpoints) {
        if (endpoint.module != top || endpoint.port != port) continue;
        if (found || endpoints.size() != 1 || endpoint.fieldID.value_or(0) != 0 ||
            channel.kind != ChannelKind::Pipe) {
          error = "SRAM top passthrough requires unique scalar PipeChannel endpoints";
          return nullptr;
        }
        found = &channel;
      }
    }
    return found;
  };
  for (Operation &op : *top.getBodyBlock()) {
    Value dest, src;
    if (auto connect = dyn_cast<ConnectOp>(op)) {
      dest = connect.getDest(); src = connect.getSrc();
    } else if (auto connect = dyn_cast<StrictConnectOp>(op)) {
      dest = connect.getDest(); src = connect.getSrc();
    } else continue;
    auto output = dyn_cast<BlockArgument>(dest), input = dyn_cast<BlockArgument>(src);
    if (!output || !input || output.getOwner() != top.getBodyBlock() ||
        input.getOwner() != top.getBodyBlock() || isa<ClockType>(dest.getType())) continue;
    auto sourceChannel = endpointChannel(output.getArgNumber(), true);
    if (!error.empty()) return std::nullopt;
    if (!sourceChannel) continue; // Not a transformed bridge passthrough.
    auto sinkChannel = endpointChannel(input.getArgNumber(), false);
    if (!error.empty()) return std::nullopt;
    if (!sinkChannel || top.getPortDirection(output.getArgNumber()) != Direction::Out ||
        top.getPortDirection(input.getArgNumber()) != Direction::In ||
        !isa<UIntType, SIntType>(src.getType()) || src.getType() != dest.getType() ||
        !src.hasOneUse() || !dest.hasOneUse() ||
        !claimed.insert(input.getArgNumber()).second ||
        !claimed.insert(output.getArgNumber()).second ||
        llvm::any_of(hierarchy.connections, [&](const TopPortConnection &connection) {
          return connection.topPort == input.getArgNumber() ||
                 connection.topPort == output.getArgNumber();
        })) {
      error = "SRAM top passthrough requires an exclusive ground input-to-output connection";
      return std::nullopt;
    }
    auto bit = UIntType::get(context, 1);
    auto type = BundleType::get(context,
        {{StringAttr::get(context, "ready"), true, bit},
         {StringAttr::get(context, "valid"), false, bit},
         {StringAttr::get(context, "bits"), false, cast<FIRRTLBaseType>(src.getType())}});
    auto portInfo = [&](unsigned port, StringRef name, Direction direction) -> std::optional<PortInfo> {
      if (!names.insert(name.str()).second) {
        error = "SRAM top passthrough channel name already exists: " + name.str();
        return std::nullopt;
      }
      SmallVector<Annotation> annotations;
      SmallVector<circt::hw::InnerSymPropertiesAttr> symbols;
      if (failed(collectFAMEWrapperPayloadMetadata(top, port, type, {}, annotations, symbols, error)))
        return std::nullopt;
      PortInfo info(StringAttr::get(context, name), type, direction);
      info.annotations = AnnotationSet(annotations, context);
      if (!symbols.empty()) info.sym = circt::hw::InnerSymAttr::get(context, symbols);
      return info;
    };
    auto sink = portInfo(input.getArgNumber(), sinkChannel->name + "_sink", Direction::In);
    auto source = portInfo(output.getArgNumber(), sourceChannel->name + "_source", Direction::Out);
    if (!sink || !source) return std::nullopt;
    auto oldInput = top.getPortName(input.getArgNumber()).str();
    auto oldOutput = top.getPortName(output.getArgNumber()).str();
    auto prefix = "~" + top->getParentOfType<CircuitOp>().getName().str() + "|" + top.getName().str() + ">";
    renames[prefix + oldInput] = prefix + sink->getName().str() + ".bits";
    renames[prefix + oldOutput] = prefix + source->getName().str() + ".bits";
    result.push_back({oldInput, oldOutput, *sink, *source, &op});
  }
  return result;
}

void rewriteSRAMTopPassthroughs(FModuleOp top, ArrayRef<SRAMTopPassthrough> passthroughs) {
  for (const auto &p : passthroughs) {
    unsigned end = top.getNumPorts();
    top.insertPorts({{end, p.sink}, {end, p.source}});
    auto *block = top.getBodyBlock();
    OpBuilder builder(p.connect);
    builder.create<ConnectOp>(p.connect->getLoc(), block->getArgument(end + 1), block->getArgument(end));
    p.connect->erase();
    llvm::BitVector erase(top.getNumPorts());
    for (unsigned i = 0; i < end; ++i)
      if (top.getPortName(i) == p.input || top.getPortName(i) == p.output) erase.set(i);
    top.erasePorts(erase);
  }
}

LogicalResult rewriteSRAMFAMEImpl(CircuitOp circuit, unsigned &rewritten,
                                  std::string &error, bool withParent = false,
                                  bool withQueues = true) {
  using namespace goldengate;
  if (withParent) {
    // Promotion can leave repeated parent writes. Resolve last-connect order
    // before following clock aliases, as the SFC LowForm preparation does.
    PassManager normalization(circuit.getContext(), CircuitOp::getOperationName());
    normalization.addNestedPass<FModuleOp>(createExpandWhensPass());
    if (failed(normalization.run(circuit))) {
      error = "SRAM transport last-connect normalization failed"; return failure();
    }
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  auto dependencies = analyzeSRAMModelDependencies(circuit, error);
  if (!raw || !hierarchy || !dependencies) return failure();
  SmallVector<FModuleOp> models;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::ModelReadPort) &&
        !anno.isClass(AnnotationClasses::ModelWritePort) &&
        !anno.isClass(AnnotationClasses::ModelReadWritePort)) continue;
    auto addr = anno.getMember<StringAttr>("addr");
    auto target = addr ? resolveAnnotationTarget(circuit, addr.getValue(), error)
                       : std::nullopt;
    auto model = target ? dyn_cast<FModuleOp>(target->module.getOperation()) : FModuleOp();
    if (!model || !target->port) {
      error = "SRAM FAME requires prepared scalar memory-port targets";
      return failure();
    }
    if (!llvm::is_contained(models, model)) models.push_back(model);
  }
  if (models.empty()) {
    error = "SRAM FAME requires at least one prepared memory model";
    return failure();
  }
  for (auto model : models) {
    unsigned uses = 0;
    bool promoted = true;
    circuit.walk([&](InstanceOp instance) {
      if (instance.getModuleName() != model.getName()) return;
      ++uses;
      promoted &= instance->getParentOfType<FModuleOp>() == hierarchy->top;
    });
    if (!uses || !promoted) {
      error = "SRAM FAME data channels require directly promoted instances: " +
              model.getName().str();
      return failure();
    }
  }
  SmallVector<ModelPortGroup> groups;
  SmallVector<GGChannelConnection, 0> channels;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::ChannelPorts)) {
      auto group = analyzeModelPortGroup(circuit, anno, error);
      if (!group) return failure();
      groups.push_back(std::move(*group));
    } else if (anno.isClass(AnnotationClasses::ChannelConnection)) {
      auto channel = analyzeChannelConnection(circuit, anno, error);
      if (!channel) return failure();
      channels.push_back(std::move(*channel));
    }
  }
  SmallVector<ModelChannelBinding> bindings;
  for (const auto &channel : channels) {
    auto bound = bindChannelToModels(channel, *hierarchy, groups, error);
    if (!bound) return failure();
    bindings.append(bound->begin(), bound->end());
  }
  // Capture the parent hub's clock and data-domain identities while ports are
  // scalar. The SRAM-only boundary keeps its historical virtual-clock scope.
  FModuleOp hub;
  std::string hubInstance, clockLocal;
  SmallVector<FAMEHubClockDomain> domains;
  SmallVector<FAMEChannelClockDomain> assignments;
  std::optional<FAMETopChannelPort> clockPort;
  if (withParent) {
    for (const auto &channel : channels) {
      if (channel.kind != ChannelKind::TargetClock && channel.kind != ChannelKind::Pipe) {
        error = "SRAM transport requires pipe data channels";
        return failure();
      }
      if (channel.kind != ChannelKind::TargetClock) continue;
      if (hub) { error = "SRAM transport requires one clock hub"; return failure(); }
      auto bound = bindChannelToModels(channel, *hierarchy, groups, error);
      if (!bound) return failure();
      if (bound->size() != 1) {
        error = "SRAM transport clock channel must bind exactly one parent hub";
        return failure();
      }
      auto boundModule = bound->front().portGroup->module;
      hub = dyn_cast<FModuleOp>(boundModule.getOperation());
      if (!hub || llvm::is_contained(models, hub)) {
        error = "SRAM transport requires a separate parent clock hub"; return failure();
      }
      hubInstance = bound->front().instance.getName().str();
      clockLocal = bound->front().portGroup->name;
      auto analyzed = analyzeFAMEHubClockDomains(channel, *hierarchy, bound->front(), error);
      if (!analyzed) return failure();
      domains = *analyzed;
      auto clockAssignments = analyzeFAMEChannelClockDomains(circuit, hub, domains, error);
      if (!clockAssignments) return failure();
      assignments = *clockAssignments;
    }
    if (!hub) { error = "SRAM transport has no parent clock hub"; return failure(); }
    unsigned uses = 0;
    circuit.walk([&](InstanceOp instance) {
      if (instance.getModuleName() == hub.getName()) ++uses;
    });
    if (uses != 1) { error = "SRAM transport requires one promoted parent hub instance"; return failure(); }
    models.push_back(hub);
  }
  SmallVector<FModuleLike> selected(models.begin(), models.end());
  auto plan = analyzeFAMEPorts(*hierarchy, bindings, channels, selected, error);
  if (!plan) return failure();
  SmallVector<SRAMDataPort, 0> ports;
  std::map<std::string, std::string> renames;
  SmallVector<SRAMTopPassthrough> passthroughs;
  if (withParent) {
    auto analyzed = analyzeSRAMTopPassthroughs(*hierarchy, channels, renames, error);
    if (!analyzed) return failure();
    passthroughs = std::move(*analyzed);
  }
  auto target = [&](FModuleOp module, StringRef port) {
    return "~" + circuit.getName().str() + "|" + module.getName().str() + ">" + port.str();
  };
  for (const auto &list : {plan->sinks, plan->sources})
    for (const auto &port : list) {
      auto binding = *port.binding;
      auto group = *binding.portGroup;
      auto model = cast<FModuleOp>(group.module.getOperation());
      if (model == hub && llvm::all_of(group.ports, [&](unsigned i) {
            return isa<ClockType>(model.getPortType(i));
          })) {
        if (clockPort) { error = "SRAM transport has multiple clock groups"; return failure(); }
        clockPort = port;
        // CombinationalPath also records exported clock aliases. SFC transfers
        // clock-channel input references to bits, while retaining historical
        // output-clock identities after the ancillary ports are internalized.
        for (unsigned index : group.ports)
          renames[target(model, model.getPortName(index))] =
              target(model, group.name + "_sink") + ".bits";
        for (const auto &connection : hierarchy->connections)
          if (connection.instance == binding.instance &&
              llvm::is_contained(group.ports, connection.instancePort))
            renames[target(hierarchy->top, hierarchy->top.getPortName(connection.topPort))] =
                target(hierarchy->top, port.portName) + ".bits";
        continue;
      }
      if ((model != hub && (group.ports.size() != 1 || group.clockPort)) ||
          binding.instancePorts.size() != group.ports.size() ||
          !llvm::all_of(group.ports, [&](unsigned index) {
            return isa<UIntType, SIntType>(model.getPortType(index));
          })) {
        error = "SRAM FAME requires ground integer parent channels and scalar virtual-clock SRAM channels";
        return failure();
      }
      std::string clock;
      if (model == hub) {
        for (const auto &assignment : assignments)
          if (assignment.globalName == binding.globalName && assignment.localName == group.name &&
              assignment.direction == group.direction) clock = assignment.modelClockName;
        if (clock.empty()) { error = "SRAM parent data channel lacks a clock domain"; return failure(); }
      }
      SmallVector<std::string> oldPorts;
      // SFC hostDecouplingRenames uses an ordered payload field for each
      // member of a multiport channel, independently at the top and model.
      auto suffix = [&](StringRef name, StringRef channel) {
        if (group.ports.size() == 1) return std::string();
        while (!name.empty() && !channel.empty() && name.front() == channel.front()) {
          name = name.drop_front();
          channel = channel.drop_front();
        }
        return "." + name.str();
      };
      for (unsigned index : group.ports) {
        auto oldPort = model.getPortName(index);
        oldPorts.push_back(oldPort.str());
        renames[target(model, oldPort)] = target(model, group.name +
            (group.direction == Direction::In ? "_sink" : "_source")) +
            ".bits" + suffix(oldPort, group.name);
        for (const auto &connection : hierarchy->connections)
          if (connection.instance == binding.instance && connection.instancePort == index) {
            auto oldTop = hierarchy->top.getPortName(connection.topPort);
            renames[target(hierarchy->top, oldTop)] = target(hierarchy->top, port.portName) +
                ".bits" + suffix(oldTop, binding.globalName);
          }
      }
      ports.push_back({model.getName().str(), binding.instance.getName().str(),
                       group.name, binding.globalName, port.portName, clock,
                       std::move(oldPorts), group.direction, port.type});
    }
  // Every data port must belong to exactly one local channel. The clock-only
  // substep validates the absence of explicit SRAM clock-channel associations.
  SmallVector<std::string> unusedHubOutputs;
  for (auto model : models) {
    auto selection = analyzeFAMEDataSelection(circuit, model, error);
    if (!selection) return failure();
    unsigned dataPorts = 0;
    for (auto port : model.getPorts()) dataPorts += !isa<ClockType>(port.type);
    std::map<std::string, std::set<std::string>> selectedPorts;
    for (const auto &port : ports) if (port.model == model.getName())
      for (const auto &oldPort : port.oldPorts)
        if (!selectedPorts[port.instance].insert(oldPort).second) {
          error = "SRAM FAME has duplicate instance data channel bindings";
          return failure();
        }
    if (model == hub) {
      for (auto p : model.getPorts())
        if (!isa<ClockType>(p.type) && p.direction == Direction::Out &&
            !selectedPorts[hubInstance].count(p.getName().str())) {
          unusedHubOutputs.push_back(p.getName().str());
          --dataPorts;
        }
    }
    for (auto instance : hierarchy->top.getOps<InstanceOp>()) {
      if (instance.getModuleName() != model.getName()) continue;
      if (selectedPorts[instance.getName().str()].size() == dataPorts) continue;
      error = "SRAM FAME requires complete, distinct data channel coverage";
      return failure();
    }
  }
  // Prepared annotations use canonical local targets. Only the SFC memory,
  // channel and DontTouch schemas have a defined payload-transfer policy here.
  // Unknown target-bearing metadata must not be silently rewritten as text.
  std::function<bool(Attribute)> referencesData = [&](Attribute attr) {
    if (auto spelling = dyn_cast<StringAttr>(attr))
      return renames.count(spelling.getValue().str()) != 0;
    if (auto array = dyn_cast<ArrayAttr>(attr))
      return llvm::any_of(array, referencesData);
    if (auto dictionary = dyn_cast<DictionaryAttr>(attr))
      return llvm::any_of(dictionary, [&](NamedAttribute member) {
        return referencesData(member.getValue());
      });
    return false;
  };
  for (auto attr : raw) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::CombinationalPath) && referencesData(attr)) {
      // CheckCombLoops emits an ordered source list and a sink, then SFC's
      // hostDecouplingRenames transfers those references to channel payloads.
      // Validate this schema before permitting the recursive target transfer;
      // other strings in an unknown annotation still have no rename policy.
      auto sink = anno.getMember<StringAttr>("sink");
      auto sources = anno.getMember<ArrayAttr>("sources");
      auto validPort = [&](Attribute value, Direction direction) {
        auto spelling = dyn_cast<StringAttr>(value);
        auto target = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error)
                               : std::nullopt;
        return target && target->port && target->fieldID.value_or(0) == 0 &&
               target->module.getPortDirection(*target->port) == direction &&
               isa<UIntType, SIntType, ClockType>(target->module.getPortType(*target->port));
      };
      if (cast<DictionaryAttr>(attr).size() != 3 || !sink || !sources || !validPort(sink, Direction::Out) ||
          !llvm::all_of(sources, [&](Attribute source) { return validPort(source, Direction::In); })) {
        error = "SRAM FAME CombinationalPath needs a ground output sink and input sources";
        return failure();
      }
    }
    if (referencesData(attr) && !anno.isClass(AnnotationClasses::DontTouch) &&
        !anno.isClass(AnnotationClasses::CombinationalPath) &&
        !anno.isClass(AnnotationClasses::ChannelPorts) &&
        !anno.isClass(AnnotationClasses::ChannelConnection) &&
        !anno.isClass(AnnotationClasses::ModelReadPort) &&
        !anno.isClass(AnnotationClasses::ModelWritePort) &&
        !anno.isClass(AnnotationClasses::ModelReadWritePort)) {
      error = "SRAM FAME data target has unsupported retained annotation metadata";
      return failure();
    }
  }
  // Clock rewriting transfers clock endpoint annotations itself. Do not later
  // replay the scalar snapshot and undo those schema-aware transfers.
  SmallVector<FAMEHubClockControl> controls;
  if (withParent) {
    if (!clockPort || failed(addFAMEHostControl(circuit, hub, error))) return failure();
    auto current = analyzeTopHierarchy(circuit, error);
    if (!current) return failure();
    InstanceOp instance;
    for (auto i : current->top.getOps<InstanceOp>()) if (i.getName() == hubInstance) instance = i;
    ModelPortGroup clockGroup = *clockPort->binding->portGroup;
    ModelChannelBinding clockBinding = *clockPort->binding;
    for (auto &index : clockGroup.ports) index += 2;
    for (auto &index : clockBinding.instancePorts) index += 2;
    for (auto &domain : domains) domain.modelPort += 2;
    clockBinding.instance = instance;
    clockBinding.portGroup = &clockGroup;
    FAMETopChannelPort liveClock{&clockBinding, clockPort->portName, clockPort->type};
    if (failed(rewriteFAMEHubClockChannel(*current, liveClock, domains, false, error))) return failure();
    auto clockControls = constructFAMEHubClockControls(circuit, hub, clockLocal, domains, error);
    if (!clockControls) return failure();
    controls = *clockControls;
    raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  }
  if (failed(rewriteSRAMVirtualClocks(circuit, rewritten, error))) return failure();
  std::set<std::pair<std::string, std::string>> rewrittenChannels;
  for (const auto &port : ports) {
    // Transform the definition once, then rewire every remaining instance at
    // the same stable channel index. Each helper changes one instance operation;
    // refresh hierarchy after every call rather than retaining erased handles.
    bool rewriteModel = rewrittenChannels.insert({port.model, port.local}).second;
    auto current = analyzeTopHierarchy(circuit, error);
    if (!current) return failure();
    FModuleOp model;
    for (auto candidate : circuit.getOps<FModuleOp>())
      if (candidate.getName() == port.model) model = candidate;
    InstanceOp instance;
    for (auto candidate : current->top.getOps<InstanceOp>())
      if (candidate.getName() == port.instance && candidate.getModuleName() == port.model)
        instance = candidate;
    SmallVector<unsigned> indices;
    for (const auto &oldPort : port.oldPorts) {
      for (unsigned i = 0; i < model.getNumPorts(); ++i)
        if (model.getPortName(i) == (rewriteModel ? oldPort : port.local +
            (port.direction == Direction::In ? "_sink" : "_source"))) {
          indices.push_back(i);
          break;
        }
    }
    if (!instance || indices.size() != port.oldPorts.size()) {
      error = "SRAM FAME lost a model channel binding during channelization";
      return failure();
    }
    ModelPortGroup group{port.local, model, port.direction, std::nullopt, indices};
    ModelChannelBinding binding{port.global, &group, instance, indices};
    FAMETopChannelPort channel{&binding, port.topPort, port.type};
    if (failed(port.direction == Direction::In
                   ? rewriteFAMEInputChannel(*current, channel, error, rewriteModel)
                   : rewriteFAMEOutputChannel(*current, channel, error, rewriteModel))) return failure();
  }
  // SFC removes the transformed models' DontTouch annotations, then applies
  // hostDecouplingRenames to memory metadata, local groups and global endpoints.
  std::function<Attribute(Attribute)> rename = [&](Attribute attr) -> Attribute {
    if (auto spelling = dyn_cast<StringAttr>(attr)) {
      auto found = renames.find(spelling.getValue().str());
      return found == renames.end() ? attr : StringAttr::get(circuit.getContext(), found->second);
    }
    if (auto array = dyn_cast<ArrayAttr>(attr)) {
      SmallVector<Attribute> values;
      for (auto value : array) values.push_back(rename(value));
      return ArrayAttr::get(circuit.getContext(), values);
    }
    if (auto dictionary = dyn_cast<DictionaryAttr>(attr)) {
      SmallVector<NamedAttribute> values;
      for (auto member : dictionary)
        values.emplace_back(member.getName(), rename(member.getValue()));
      return DictionaryAttr::get(circuit.getContext(), values);
    }
    return attr;
  };
  SmallVector<Attribute> updated;
  for (auto attr : raw) {
    Annotation anno(attr);
    auto spelling = anno.getMember<StringAttr>("target");
    if (anno.isClass(AnnotationClasses::DontTouch) && spelling &&
        llvm::any_of(models, [&](FModuleOp model) {
          return spelling.getValue().starts_with(target(model, ""));
        })) continue;
    updated.push_back(rename(attr));
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), updated));
  for (auto model : models) {
    SmallVector<std::string> inputs, outputs;
    SmallVector<FAMEFiredChannel> fired;
    SmallVector<LocalChannelDependency> rules;
    OpBuilder builder(model.getBodyBlock(), model.getBodyBlock()->begin());
    Value one = builder.create<ConstantOp>(model.getLoc(), UIntType::get(model.getContext(), 1), APInt(1, 1));
    std::string instance;
    std::set<std::string> localChannels;
    for (const auto &port : ports) if (port.model == model.getName()) {
      instance = port.instance;
      if (!localChannels.insert(port.local).second) continue;
      bool input = port.direction == Direction::In;
      (input ? inputs : outputs).push_back(port.local);
      Value enable = one;
      if (model == hub) {
        for (const auto &control : controls) if (control.modelClockName == port.clock)
          enable = input ? control.inputEnable : control.outputEnable;
      }
      fired.push_back({port.local, input, enable, model == hub});
    }
    for (const auto &dependency : *dependencies)
      if (dependency.module == model) rules.append(dependency.outputs.begin(), dependency.outputs.end());
    if (failed(ensureFAMEFiredRegisters(model, fired, error)) ||
        failed(rewriteFAMEFiredStates(model, fired, error)) ||
        failed(rewriteFAMEInputReadies(model, inputs, error)) ||
        failed(rewriteFAMEOutputValids(model, rules, error)) ||
        failed(rewriteFAMEFinishing(model, inputs, outputs, model == hub ? clockLocal : "", error)))
      return failure();
    if (model == hub) {
      if (failed(internalizeFAMEOutputClocks(hierarchy->top, hub, hubInstance, error))) return failure();
      SmallVector<StringRef> unused;
      for (const auto &name : unusedHubOutputs) unused.push_back(name);
      if (failed(internalizeFAMEUnusedOutputs(circuit, hub, unused, error))) return failure();
    }
    if (failed(groupFAMEChannelPorts(hierarchy->top, model, instance,
                                    model == hub ? clockLocal + "_sink" : "", error))) return failure();
  }
  if (withParent) {
    rewriteSRAMTopPassthroughs(hierarchy->top, passthroughs);
    if (failed(removeFAMEStaleTopClocks(hierarchy->top, error))) return failure();
    if (withQueues && (failed(addRemainingFanoutAnnotations(circuit, error)) ||
        failed(addFAMEBoundaryPipeChannels(circuit, error)) ||
        failed(addFAMEPipeWrapper(circuit, error)) ||
        failed(addFAMEClockChannel(circuit, error)) ||
        failed(activateFAMEPipeWrapper(circuit, error)))) return failure();
  }
  return success();
}
} // namespace

static LogicalResult rewriteSRAMBoundary(CircuitOp circuit, unsigned &rewritten,
                                         std::string &error, bool transport,
                                         bool queues = true) {
  rewritten = 0;
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  unsigned count = 0;
  if (failed(rewriteSRAMFAMEImpl(*staged, count, error, transport, queues))) return failure();
  if (failed(verify(*staged))) {
    error = "SRAM FAME produced invalid FIRRTL IR";
    return failure();
  }
  circuit->setAttrs((*staged)->getAttrs());
  circuit.getBody().takeBody(staged->getBody());
  rewritten = count;
  return success();
}

LogicalResult goldengate::rewriteSRAMFAME(CircuitOp circuit, unsigned &rewritten,
                                         std::string &error) {
  return rewriteSRAMBoundary(circuit, rewritten, error, false);
}

LogicalResult goldengate::rewriteSRAMPipeTransport(CircuitOp circuit, unsigned &rewritten,
                                                  std::string &error) {
  return rewriteSRAMBoundary(circuit, rewritten, error, true);
}

LogicalResult goldengate::rewriteSRAMParentFAME(CircuitOp circuit, unsigned &rewritten,
                                               std::string &error) {
  return rewriteSRAMBoundary(circuit, rewritten, error, true, false);
}

/// Required input invariants: prepared ground integer parent channels and scalar
/// SRAM channels, supported read/write payloads and one XDC circuit path mapping.
/// Annotations consumed: XDC paths/snippets; memory/channel targets transfer
/// through FAME and remain on the adapter ports. Annotations produced: XDC
/// output files. IR mutations: FAME state, queues and async RAM implementations.
/// Analyses required: hierarchy, channel binding, dependencies and typed targets.
/// Analyses preserved: none. Output invariants: verified executable memory
/// transport; only surviving hub gates contribute generated-clock constraints.
LogicalResult goldengate::rewriteSRAMTimingModels(
    CircuitOp circuit, unsigned &rewritten, std::string &error) {
  rewritten = 0;
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  auto raw = (*staged)->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "SRAM timing models require retained memory annotations";
    return failure();
  }
  SmallVector<std::string> names;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::ModelReadPort) &&
        !anno.isClass(AnnotationClasses::ModelWritePort) &&
        !anno.isClass(AnnotationClasses::ModelReadWritePort)) continue;
    auto addr = anno.getMember<StringAttr>("addr");
    auto target = addr ? resolveAnnotationTarget(*staged, addr.getValue(), error)
                       : std::nullopt;
    if (!target || !isa<FModuleOp>(target->module.getOperation())) return failure();
    auto name = target->module.getName().str();
    if (!llvm::is_contained(names, name)) names.push_back(std::move(name));
  }
  unsigned count = 0;
  if (failed(rewriteSRAMFAMEImpl(*staged, count, error, true, true))) return failure();
  for (const auto &name : names) {
    FModuleOp wrapper, implementation;
    for (auto model : staged->getOps<FModuleOp>())
      if (model.getName() == name) wrapper = model;
    RAMModelParameters parameters;
    if (failed(materializeRAMModel(*staged, wrapper, implementation, parameters, error)))
      return failure();
  }
  // Resolve constraints while native gate identity and the complete transport
  // hierarchy still exist. FIRRTL text export cannot retain these attributes.
  if (failed(prepareXDCOutput(*staged, error))) return failure();
  if (failed(verify(*staged))) {
    error = "SRAM timing models produced invalid FIRRTL IR";
    return failure();
  }
  circuit->setAttrs((*staged)->getAttrs());
  circuit.getBody().takeBody(staged->getBody());
  rewritten = names.size();
  return success();
}
