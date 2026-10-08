// See LICENSE for license details.
#include "goldengate/SRAMModelChannels.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/ChannelExcision.h"
#include "goldengate/ExtractModel.h"
#include "goldengate/FAMEDefaults.h"
#include "goldengate/FAMEHostControl.h"
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
#include "circt/Dialect/FIRRTL/Passes.h"
#include "mlir/IR/Verifier.h"
#include "mlir/IR/Builders.h"
#include "mlir/Pass/PassManager.h"
#include <functional>
#include <map>

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
  std::string model, instance, oldPort, local, global, topPort;
  Direction direction;
  BundleType type;
};

LogicalResult rewriteSRAMFAMEImpl(CircuitOp circuit, unsigned &rewritten,
                                  std::string &error) {
  using namespace goldengate;
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
    if (uses != 1 || !promoted) {
      error = "SRAM FAME data channels require one promoted instance per definition: " +
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
  SmallVector<FModuleLike> selected(models.begin(), models.end());
  auto plan = analyzeFAMEPorts(*hierarchy, bindings, channels, selected, error);
  if (!plan) return failure();
  SmallVector<SRAMDataPort, 0> ports;
  std::map<std::string, std::string> renames;
  auto target = [&](FModuleOp module, StringRef port) {
    return "~" + circuit.getName().str() + "|" + module.getName().str() + ">" + port.str();
  };
  for (const auto &list : {plan->sinks, plan->sources})
    for (const auto &port : list) {
      auto binding = *port.binding;
      auto group = *binding.portGroup;
      auto model = cast<FModuleOp>(group.module.getOperation());
      if (group.ports.size() != 1 || group.clockPort || binding.instancePorts.size() != 1 ||
          !isa<UIntType, SIntType>(model.getPortType(group.ports[0]))) {
        error = "SRAM FAME currently requires scalar integer virtual-clock data channels";
        return failure();
      }
      auto oldPort = model.getPortName(group.ports[0]).str();
      ports.push_back({model.getName().str(), binding.instance.getName().str(), oldPort,
                       group.name, binding.globalName, port.portName, group.direction, port.type});
      renames[target(model, oldPort)] = target(model, group.name +
          (group.direction == Direction::In ? "_sink" : "_source")) + ".bits";
      for (const auto &connection : hierarchy->connections)
        if (connection.instance == binding.instance &&
            connection.instancePort == group.ports[0])
          renames[target(hierarchy->top, hierarchy->top.getPortName(connection.topPort))] =
              target(hierarchy->top, port.portName) + ".bits";
    }
  // Every data port must belong to exactly one local channel. The clock-only
  // substep validates the absence of explicit SRAM clock-channel associations.
  for (auto model : models) {
    auto selection = analyzeFAMEDataSelection(circuit, model, error);
    if (!selection) return failure();
    unsigned dataPorts = 0, selectedPorts = 0;
    for (auto port : model.getPorts()) dataPorts += !isa<ClockType>(port.type);
    for (const auto &port : ports) selectedPorts += port.model == model.getName();
    if (dataPorts != selectedPorts) {
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
    if (referencesData(attr) && !anno.isClass(AnnotationClasses::DontTouch) &&
        !anno.isClass(AnnotationClasses::ChannelPorts) &&
        !anno.isClass(AnnotationClasses::ChannelConnection) &&
        !anno.isClass(AnnotationClasses::ModelReadPort) &&
        !anno.isClass(AnnotationClasses::ModelWritePort) &&
        !anno.isClass(AnnotationClasses::ModelReadWritePort)) {
      error = "SRAM FAME data target has unsupported retained annotation metadata";
      return failure();
    }
  }
  if (failed(rewriteSRAMVirtualClocks(circuit, rewritten, error))) return failure();
  for (const auto &port : ports) {
    auto current = analyzeTopHierarchy(circuit, error);
    if (!current) return failure();
    FModuleOp model;
    for (auto candidate : circuit.getOps<FModuleOp>())
      if (candidate.getName() == port.model) model = candidate;
    InstanceOp instance;
    for (auto candidate : current->top.getOps<InstanceOp>())
      if (candidate.getName() == port.instance && candidate.getModuleName() == port.model)
        instance = candidate;
    std::optional<unsigned> index;
    for (unsigned i = 0; i < model.getNumPorts(); ++i)
      if (model.getPortName(i) == port.oldPort) index = i;
    if (!instance || !index) {
      error = "SRAM FAME lost a scalar model binding during channelization";
      return failure();
    }
    ModelPortGroup group{port.local, model, port.direction, std::nullopt, {*index}};
    ModelChannelBinding binding{port.global, &group, instance, {*index}};
    FAMETopChannelPort channel{&binding, port.topPort, port.type};
    if (failed(port.direction == Direction::In
                   ? rewriteFAMEInputChannel(*current, channel, error)
                   : rewriteFAMEOutputChannel(*current, channel, error))) return failure();
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
    for (const auto &port : ports) if (port.model == model.getName()) {
      bool input = port.direction == Direction::In;
      (input ? inputs : outputs).push_back(port.local);
      fired.push_back({port.local, input, one, false});
      instance = port.instance;
    }
    for (const auto &dependency : *dependencies)
      if (dependency.module == model) rules.append(dependency.outputs.begin(), dependency.outputs.end());
    if (failed(ensureFAMEFiredRegisters(model, fired, error)) ||
        failed(rewriteFAMEFiredStates(model, fired, error)) ||
        failed(rewriteFAMEInputReadies(model, inputs, error)) ||
        failed(rewriteFAMEOutputValids(model, rules, error)) ||
        failed(rewriteFAMEFinishing(model, inputs, outputs, "", error)) ||
        failed(groupFAMEChannelPorts(hierarchy->top, model, instance, "", error))) return failure();
  }
  return success();
}
} // namespace

LogicalResult goldengate::rewriteSRAMFAME(CircuitOp circuit, unsigned &rewritten,
                                         std::string &error) {
  rewritten = 0;
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  unsigned count = 0;
  if (failed(rewriteSRAMFAMEImpl(*staged, count, error))) return failure();
  if (failed(verify(*staged))) {
    error = "SRAM FAME produced invalid FIRRTL IR";
    return failure();
  }
  circuit->setAttrs((*staged)->getAttrs());
  circuit.getBody().takeBody(staged->getBody());
  rewritten = count;
  return success();
}
