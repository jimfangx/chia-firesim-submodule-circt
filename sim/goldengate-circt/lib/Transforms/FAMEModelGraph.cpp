// See LICENSE for license details.
#include "goldengate/FAMEModelGraph.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/FAMEAnnotations.h"
#include "goldengate/FAMEHostControl.h"
#include "goldengate/FAMEClockEnable.h"
#include "goldengate/FAMEClockChannel.h"
#include "goldengate/FAMEInputChannel.h"
#include "goldengate/FAMEOutputChannel.h"
#include "goldengate/FAMEModelControls.h"
#include "goldengate/FAMEPipeChannel.h"
#include "goldengate/FAMEReadyValidChannel.h"
#include "goldengate/RemainingFanout.h"
#include "goldengate/TargetUtils.h"
#include "goldengate/HierarchicalTargets.h"
#include "goldengate/RetainedAnnotationTargets.h"
#include "FAMEPortAnnotations.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Verifier.h"
#include <functional>
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;
using namespace goldengate;
namespace {
struct DataEndpoint {
  std::string model, instance, local, global, topPort;
  SmallVector<std::string> oldPorts;
  Direction direction;
  BundleType type;
};
struct Model {
  FModuleOp op;
  FAMEDataSelection selection;
  SmallVector<FAMEChannelClockDomain> assignments;
  SmallVector<std::string> instances, unusedOutputs;
  std::map<std::string, DictionaryAttr> instanceMetadata;
  std::string targetClock;
};
FModuleOp find(CircuitOp circuit, StringRef name) {
  for (auto model : circuit.getOps<FModuleOp>()) if (model.getName() == name) return model;
  return {};
}
std::optional<unsigned> port(FModuleOp model, StringRef name) {
  for (unsigned i = 0; i < model.getNumPorts(); ++i) if (model.getPortName(i) == name) return i;
  return std::nullopt;
}
InstanceOp instance(FModuleOp top, StringRef name) {
  for (auto value : top.getOps<InstanceOp>()) if (value.getName() == name) return value;
  return {};
}
// During one shared-definition channel rewrite, instances still carrying leaf
// ports temporarily differ in port count from the collapsed definition. The
// graph was validated before mutation; capture only direct physical connections
// here, until every retained instance reaches the common ABI. Primitive checks
// retain exclusive wiring/leaf type checks, and the atomic commit verifies the
// complete instance graph once all channel rewrites have finished.
TopHierarchy captureChannelRewriteConnections(FModuleOp top) {
  TopHierarchy result{top, {}};
  top.walk([&](Operation *op) {
    Value dest, src;
    if (auto c=dyn_cast<ConnectOp>(op)) { dest=c.getDest(); src=c.getSrc(); }
    else if (auto c=dyn_cast<StrictConnectOp>(op)) { dest=c.getDest(); src=c.getSrc(); }
    else return;
    auto argument=dyn_cast<BlockArgument>(dest); auto value=dyn_cast<OpResult>(src);
    if (!argument || !value) { argument=dyn_cast<BlockArgument>(src); value=dyn_cast<OpResult>(dest); }
    if (!argument || !value || argument.getOwner()!=top.getBodyBlock()) return;
    auto child=dyn_cast<InstanceOp>(value.getOwner());
    if (child && child->getParentOfType<FModuleOp>()==top)
      result.connections.push_back({argument.getArgNumber(),child,value.getResultNumber()});
  });
  return result;
}
// FAMETransform promotes bridge-to-bridge identities to complete Decoupled
// connections, independently of model firing. Capture the old SSA endpoints
// before model rewrites invalidate port indices. Each scalar must have one
// channel and one identity use. ChannelExcision splits bridge-source fanout
// into independent annotated inputs before this boundary; do not silently
// bypass that policy by sharing an unsplit scalar with a model.
struct TopPassthrough {
  std::string input, output;
  PortInfo sink, source;
  Operation *connect;
};
std::optional<SmallVector<TopPassthrough>> analyzeTopPassthroughs(
    const TopHierarchy &hierarchy, ArrayRef<GGChannelConnection> channels,
    std::map<std::string, std::string> &renames, std::set<unsigned> &claimed,
    std::string &error) {
  auto top = hierarchy.top;
  auto *context = top.getContext();
  SmallVector<TopPassthrough> result;
  std::set<std::string> names;
  for (auto p : top.getPorts()) names.insert(p.getName().str());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.insert(name.getValue().str());
  });
  auto channelFor = [&](unsigned port, bool source) -> const GGChannelConnection * {
    const GGChannelConnection *found = nullptr;
    for (const auto &channel : channels) {
      const auto &endpoints = source ? channel.sources : channel.sinks;
      for (const auto &endpoint : endpoints) {
        if (endpoint.module != top || endpoint.port != port) continue;
        if (found || endpoints.size() != 1 || endpoint.fieldID.value_or(0) != 0 ||
            channel.kind != ChannelKind::Pipe) {
          error = "top passthrough requires unique scalar PipeChannel endpoints";
          return nullptr;
        }
        found = &channel;
      }
    }
    return found;
  };
  for (Operation &op : *top.getBodyBlock()) {
    Value dest, src;
    if (auto c = dyn_cast<ConnectOp>(op)) { dest = c.getDest(); src = c.getSrc(); }
    else if (auto c = dyn_cast<StrictConnectOp>(op)) { dest = c.getDest(); src = c.getSrc(); }
    else continue;
    auto output = dyn_cast<BlockArgument>(dest), input = dyn_cast<BlockArgument>(src);
    if (!output || !input || output.getOwner() != top.getBodyBlock() ||
        input.getOwner() != top.getBodyBlock() || isa<ClockType>(dest.getType())) continue;
    auto sourceChannel = channelFor(output.getArgNumber(), true);
    if (!error.empty()) return std::nullopt;
    if (!sourceChannel) continue;
    auto sinkChannel = channelFor(input.getArgNumber(), false);
    if (!error.empty()) return std::nullopt;
    if (!sinkChannel || top.getPortDirection(output.getArgNumber()) != Direction::Out ||
        top.getPortDirection(input.getArgNumber()) != Direction::In ||
        !isa<UIntType, SIntType>(src.getType()) || src.getType() != dest.getType() ||
        !src.hasOneUse() || !dest.hasOneUse() ||
        claimed.count(input.getArgNumber()) || claimed.count(output.getArgNumber()) ||
        llvm::any_of(hierarchy.connections, [&](const TopPortConnection &connection) {
          return connection.topPort == input.getArgNumber() || connection.topPort == output.getArgNumber();
        })) {
      error = "top passthrough requires exclusive ground input-to-output wiring; model/token fanout is unsupported";
      return std::nullopt;
    }
    auto bit = UIntType::get(context, 1);
    auto type = BundleType::get(context,
        {{StringAttr::get(context, "ready"), true, bit},
         {StringAttr::get(context, "valid"), false, bit},
         {StringAttr::get(context, "bits"), false, cast<FIRRTLBaseType>(src.getType())}});
    auto makePort = [&](unsigned index, StringRef name, Direction direction) -> std::optional<PortInfo> {
      if (!names.insert(name.str()).second) {
        error = "top passthrough channel name already exists: " + name.str();
        return std::nullopt;
      }
      SmallVector<Annotation> annotations;
      SmallVector<circt::hw::InnerSymPropertiesAttr> symbols;
      if (failed(collectFAMEWrapperPayloadMetadata(top, index, type, {}, annotations, symbols, error))) return std::nullopt;
      PortInfo info(StringAttr::get(context, name), type, direction);
      info.annotations = AnnotationSet(annotations, context);
      if (!symbols.empty()) info.sym = circt::hw::InnerSymAttr::get(context, symbols);
      return info;
    };
    auto sink = makePort(input.getArgNumber(), sinkChannel->name + "_sink", Direction::In);
    auto source = makePort(output.getArgNumber(), sourceChannel->name + "_source", Direction::Out);
    if (!sink || !source) return std::nullopt;
    auto oldInput = top.getPortName(input.getArgNumber()).str();
    auto oldOutput = top.getPortName(output.getArgNumber()).str();
    auto prefix = "~" + top->getParentOfType<CircuitOp>().getName().str() + "|" + top.getName().str() + ">";
    renames[prefix + oldInput] = prefix + sink->getName().str() + ".bits";
    renames[prefix + oldOutput] = prefix + source->getName().str() + ".bits";
    claimed.insert(input.getArgNumber()); claimed.insert(output.getArgNumber());
    result.push_back({oldInput, oldOutput, *sink, *source, &op});
  }
  return result;
}
void rewriteTopPassthroughs(FModuleOp top, ArrayRef<TopPassthrough> passthroughs) {
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
std::string suffix(StringRef name, StringRef channel, unsigned size) {
  if (size == 1) return {};
  while (!name.empty() && !channel.empty() && name.front() == channel.front()) {
    name = name.drop_front(); channel = channel.drop_front();
  }
  return "." + name.str();
}
// Clock aliases promoted out of cloned models can feed back into the hub.
// SFC's transformed model ABI retains only the clock channel, not these
// ancillary scalar inputs. Fold physical identities before taking any port
// indices: subsequent channelization and clock gating then cover every live
// consumer, including consumers below the hub's hierarchy.
LogicalResult foldHubClockAliases(CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) return success(); // The ordinary graph validator reports this.
  bool hasProofs = false;
  for (auto model : circuit.getOps<FModuleOp>()) hasProofs |= model->hasAttr("goldengate.fameClockAliases");
  if (!hasProofs) return success();
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  if (!hierarchy) return failure();
  SmallVector<ModelPortGroup> groups;
  for (auto attr : raw) {
    Annotation a(attr);
    // Data groups with an associated clock cannot be the clock-input group.
    // Avoid resolving their many payload targets just to find the hub clock.
    if (!a.isClass(AnnotationClasses::ChannelPorts) || a.getMember("clockPort")) continue;
    auto group = analyzeModelPortGroup(circuit, a, error);
    if (!group) return failure();
    groups.push_back(std::move(*group));
  }
  for (auto attr : raw) {
    Annotation a(attr);
    if (!a.isClass(AnnotationClasses::ChannelConnection)) continue;
    auto info = a.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") != AnnotationClasses::TargetClockChannel) continue;
    auto channel = analyzeChannelConnection(circuit, a, error);
    if (!channel) return failure();
    if (channel->kind != ChannelKind::TargetClock) continue;
    auto bindings = bindChannelToModels(*channel, *hierarchy, groups, error);
    if (!bindings) return failure();
    if (bindings->size() != 1) continue; // Ordinary validator diagnoses arity.
    auto &binding = bindings->front();
    auto module = binding.portGroup->module;
    auto model = dyn_cast<FModuleOp>(module.getOperation());
    if (!model) continue;
    auto domains = analyzeFAMEHubClockDomains(*channel, *hierarchy, binding, error);
    if (!domains) return failure();
    auto hub = binding.instance;
    struct Alias { std::string name, domain; };
    SmallVector<Alias> aliases;
    auto proofs = model->getAttrOfType<ArrayAttr>("goldengate.fameClockAliases");
    if (!proofs) continue;
    std::set<std::string> declared;
    for (const auto &domain : *domains) declared.insert(domain.modelClockName);
    std::set<std::string> seen;
    for (auto attr : proofs) {
      auto record = dyn_cast<DictionaryAttr>(attr);
      auto alias = record ? record.getAs<StringAttr>("port") : StringAttr();
      auto domain = record ? record.getAs<StringAttr>("clock") : StringAttr();
      auto owner = record ? record.getAs<StringAttr>("instance") : StringAttr();
      auto index = alias ? port(model, alias.getValue()) : std::nullopt;
      auto canonicalPort = domain ? port(model, domain.getValue()) : std::nullopt;
      if (!index || !canonicalPort || !owner || owner != hub.getNameAttr() ||
          !declared.count(domain.getValue().str()) || declared.count(alias.getValue().str()) ||
          !seen.insert(alias.getValue().str()).second ||
          model.getPortDirection(*index) != Direction::In ||
          !isa<ClockType>(model.getPortType(*index))) {
        error = "FAME hub clock alias proof no longer matches its input/domain identity";
        return failure();
      }
      unsigned i = *index;
      auto name = alias.getValue().str(), canonical = domain.getValue().str();
      auto annotated = [&](ArrayAttr annotations) {
        return annotations && i < annotations.size() &&
               !cast<ArrayAttr>(annotations[i]).empty();
      };
      if (annotated(model.getPortAnnotationsAttr()) || model.getPortSymbolAttr(i) ||
          annotated(hub.getPortAnnotationsAttr())) {
        error = "FAME hub clock alias has attached identity without a transfer policy: " + name;
        return failure();
      }
      for (auto &op : *model.getBodyBlock())
        if (auto existing = op.getAttrOfType<StringAttr>("name"))
          if (existing.getValue() == name) {
            error = "FAME hub clock alias wire name collides with a declaration: " + name;
            return failure();
          }
      aliases.push_back({name, canonical});
    }
    if (aliases.empty()) continue;
    std::map<std::string, std::string> clockRenames;
    auto prefix = "~" + circuit.getName().str() + "|";
    for (const auto &alias : aliases) {
      clockRenames[prefix + model.getName().str() + ">" + alias.name] =
          prefix + model.getName().str() + ">" + alias.domain;
      clockRenames[prefix + hierarchy->top.getName().str() + ">" +
                   hub.getName().str() + "." + alias.name] =
          prefix + hierarchy->top.getName().str() + ">" + hub.getName().str() + "." + alias.domain;
    }
    for (const auto &alias : aliases) {
      auto old = port(model, alias.name), canonical = port(model, alias.domain);
      if (!old || !canonical) { error = "lost FAME hub clock alias"; return failure(); }
      OpBuilder b(model.getBodyBlock(), model.getBodyBlock()->begin());
      auto wire = b.create<WireOp>(model.getLoc(), model.getPortType(*old), alias.name);
      model.getArgument(*old).replaceAllUsesWith(wire.getResult());
      b.create<StrictConnectOp>(model.getLoc(), wire.getResult(), model.getArgument(*canonical));
      if (failed(removeFAMEVirtualClockPort(circuit, model, alias.name, error))) return failure();
    }
    // Associated clock metadata follows the canonical domain. Other targets
    // retain the same local name, now a driven wire rather than an input port;
    // opaque constructor/format strings are never rewritten.
    SmallVector<Attribute> updated;
    for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      Annotation annotation(attr);
      StringRef member = annotation.isClass(AnnotationClasses::ChannelConnection) ? "clock" :
                         annotation.isClass(AnnotationClasses::ChannelPorts) ? "clockPort" : "";
      if (!member.empty()) if (auto target = annotation.getMember<StringAttr>(member)) {
        auto found = clockRenames.find(target.getValue().str());
        if (found != clockRenames.end())
          annotation.setMember(member, StringAttr::get(circuit.getContext(), found->second));
      }
      updated.push_back(annotation.getAttr());
    }
    circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), updated));
    model->removeAttr("goldengate.fameClockAliases");
    // One target-clock hub is the supported FAME model-graph contract. Its
    // removal changed instance handles/port indices; the graph starts fresh.
    return success();
  }
  return success();
}
LogicalResult rewrite(CircuitOp circuit, FAMEModelGraphResult &result,
                      std::string &error, bool withTransport, bool enableMultiThreading,
                      FAME5ClockSemantics threadingClocks) {
  auto reject = [&](const std::string &why) { error = "FAME model graph: " + why; return failure(); };
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  auto prepared = PreparedFAMEAnalysis::create(circuit, error);
  const auto *hierarchy = prepared ? &prepared->hierarchy() : nullptr;
  if (!raw || !hierarchy) return reject("requires prepared hierarchy and retained annotations: " + error);
  auto top = hierarchy->top;
  bool usedTop = false;
  circuit.walk([&](InstanceOp i) { usedTop |= i.getModuleName() == top.getName(); });
  auto hostClock = port(top, "hostClock"), hostReset = port(top, "hostReset");
  if (usedTop || !hostClock || !hostReset || top.getPortDirection(*hostClock) != Direction::In ||
      top.getPortDirection(*hostReset) != Direction::In || !isa<ClockType>(top.getPortType(*hostClock)) ||
      top.getPortType(*hostReset) != UIntType::get(circuit.getContext(), 1))
    return reject("requires an uninstantiated top with hostClock and hostReset inputs");
  SmallVector<Model, 0> models;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::FAMETransform)) continue;
    auto spelling = anno.getMember<StringAttr>("target");
    auto resolved = spelling ? resolveAnnotationTarget(circuit, spelling.getValue(), error) : std::nullopt;
    auto op = resolved && !resolved->port ? dyn_cast<FModuleOp>(resolved->module.getOperation()) : FModuleOp{};
    if (!op || op == top) return reject("invalid transformed model target");
    if (llvm::any_of(models, [&](const auto &m) { return m.op == op; })) continue;
    auto data = analyzeFAMEDataSelection(*prepared, op, error);
    if (!data) return failure();
    Model model; model.op = op; model.selection = std::move(*data);
    bool promoted = true;
    circuit.walk([&](InstanceOp i) {
      if (i.getModuleName() != op.getName()) return;
      promoted &= i->getParentOfType<FModuleOp>() == top;
      model.instances.push_back(i.getName().str());
      NamedAttrList metadata;
      for (auto attr : i->getAttrs()) {
        auto name = attr.getName().strref();
        // Shape/name attributes are reconstructed by each channel rewrite;
        // declaration identity and other instance metadata stay with this
        // retained instance even when a primitive creates a replacement op.
        if (name != "moduleName" && name != "name" && name != "portNames" &&
            name != "portDirections" && name != "portAnnotations")
          metadata.set(attr.getName(), attr.getValue());
      }
      model.instanceMetadata.emplace(i.getName().str(), metadata.getDictionary(circuit.getContext()));
    });
    if (!promoted || model.instances.empty()) return reject("models require directly promoted live instances: " + op.getName().str());
    models.push_back(std::move(model));
  }
  if (models.empty()) return reject("no transformed definitions");
  auto channels = prepared->channels();
  for (const auto &channel : channels)
    if (withTransport && channel.kind == ChannelKind::Pipe &&
        (channel.sources.size() > 1 || channel.sinks.size() > 1))
      return reject("aggregated PipeChannel transport is unsupported by SFC SimWrapper");
  if (!validateDecoupledChannelPairs(channels, error)) return failure();
  SmallVector<ModelChannelBinding> bindings;
  Model *hub = nullptr;
  std::string clockLocal, clockGlobal, clockTopPort, hubInstance;
  SmallVector<FAMEHubClockDomain> domains;
  for (auto [index, channel] : llvm::enumerate(channels)) {
    auto bound = prepared->bindingsFor(index, error); if (!bound) return failure();
    bindings.append(bound->begin(), bound->end());
    if (channel.kind != ChannelKind::TargetClock) continue;
    if (hub || bound->size() != 1) return reject("requires one clock channel bound to one hub instance");
    for (auto &model : models) if (model.op == bound->front().portGroup->module) hub = &model;
    if (!hub || hub->instances.size() != 1) return reject("clock hub must be a selected unique definition instance");
    auto captured = analyzeFAMEHubClockDomains(channel, *hierarchy, bound->front(), error);
    if (!captured) return failure();
    domains = *captured; clockLocal = bound->front().portGroup->name;
    clockGlobal = channel.name;
    auto boundInstance = bound->front().instance;
    hubInstance = boundInstance.getName().str();
  }
  // FAMETransform permits independently scheduled scalar virtual-clock models
  // without a clock-token hub. SimWrapper requires exactly one clock channel,
  // so retain that requirement only when constructing simulator transport.
  if (!hub && withTransport) return reject("simulator transport requires an explicit target clock hub");
  for (auto &model : models) {
    auto assignment = analyzeFAMEChannelClockDomains(*prepared, model.op,
        hub && model.op == hub->op ? ArrayRef<FAMEHubClockDomain>(domains) : ArrayRef<FAMEHubClockDomain>{}, error);
    if (!assignment) return failure(); model.assignments = *assignment;
    if (!hub || model.op != hub->op) {
      for (auto p : model.op.getPorts()) if (isa<ClockType>(p.type)) {
        if (p.direction == Direction::In) model.targetClock = p.getName().str();
      }
      if (model.targetClock.empty()) return reject("virtual model requires a scalar target clock");
    }
  }
  SmallVector<FModuleLike> selected;
  for (auto &model : models) selected.push_back(model.op);
  auto plan = analyzeFAMEPorts(*hierarchy, bindings, channels, selected, error); if (!plan) return failure();
  auto target = [&](FModuleOp model, StringRef name) {
    return "~" + circuit.getName().str() + "|" + model.getName().str() + ">" + name.str();
  };
  std::map<std::string, std::string> renames;
  SmallVector<DataEndpoint, 0> endpoints;
  std::set<unsigned> claimedTop;
  // This phase does not mutate signatures or connectivity. Index the stable
  // hierarchy once instead of scanning every connection for each data leaf.
  // Keep each matching connection in its original order.
  llvm::DenseMap<std::pair<Operation *, unsigned>,
                 SmallVector<const TopPortConnection *>> connectionsByPort;
  for (const auto &connection : hierarchy->connections) {
    auto instance = connection.instance;
    connectionsByPort[{instance.getOperation(), connection.instancePort}]
        .push_back(&connection);
  }
  for (const auto &list : {plan->sinks, plan->sources}) for (const auto &endpoint : list) {
    auto binding = *endpoint.binding; auto group = *binding.portGroup;
    auto model = cast<FModuleOp>(group.module.getOperation());
    if (hub && model == hub->op && group.name == clockLocal) { clockTopPort = endpoint.portName; continue; }
    if (binding.instancePorts.size() != group.ports.size() ||
        !llvm::all_of(group.ports, [&](unsigned i) { return isa<UIntType, SIntType>(model.getPortType(i)); }))
      return reject("requires ground integer channel leaves");
    SmallVector<std::string> oldPorts;
    for (unsigned i : group.ports) {
      auto oldModel = model.getPortName(i); oldPorts.push_back(oldModel.str());
      auto newModel = target(model, group.name + (group.direction == Direction::In ? "_sink" : "_source")) +
          ".bits" + suffix(oldModel, group.name, group.ports.size());
      auto [r, fresh] = renames.emplace(target(model, oldModel), newModel);
      if (!fresh && r->second != newModel) return reject("incompatible shared model payload identity");
      for (const auto *matched : connectionsByPort[{binding.instance.getOperation(), i}]) {
        const auto &connection = *matched;
        claimedTop.insert(connection.topPort);
        auto oldTop = top.getPortName(connection.topPort);
        auto field = group.direction == Direction::Out && group.ports.size() > 1
            ? getFAMEOutputAliasField(*hierarchy, endpoint, i, connection.topPort, error)
            : std::optional<std::string>(suffix(oldTop, binding.globalName, group.ports.size()));
        if (!field) return failure();
        auto fieldSuffix = group.direction == Direction::Out && !field->empty() ? "." + *field : *field;
        auto newTop = target(top, endpoint.portName) + ".bits" + fieldSuffix;
        auto [r, fresh] = renames.emplace(target(top, oldTop), newTop);
        if (!fresh && r->second != newTop) return reject("incompatible shared top payload identity");
      }
    }
    endpoints.push_back({model.getName().str(), binding.instance.getName().str(), group.name,
        binding.globalName, endpoint.portName, std::move(oldPorts), group.direction, endpoint.type});
  }
  // Coverage is checked per instance, before any model/port mutation. Unused
  // output data may be internalized only when every instance leaves it unused.
  for (auto &model : models) {
    std::map<std::string, std::set<std::string>> coverage;
    for (const auto &endpoint : endpoints) if (endpoint.model == model.op.getName())
      for (const auto &name : endpoint.oldPorts) if (!coverage[endpoint.instance].insert(name).second)
        return reject("duplicate per-instance data channel coverage");
    for (auto p : model.op.getPorts()) {
      if (isa<ClockType>(p.type)) continue;
      bool missing = false, present = false;
      for (const auto &name : model.instances) (coverage[name].count(p.getName().str()) ? present : missing) = true;
      if (missing && (present || p.direction != Direction::Out)) {
        std::string uncovered;
        for (const auto &name : model.instances)
          if (!coverage[name].count(p.getName().str())) {
            if (!uncovered.empty()) uncovered += ",";
            uncovered += name;
          }
        return reject("incomplete per-instance channel coverage: model=" +
                      model.op.getName().str() + " port=" + p.getName().str() +
                      " direction=" + (p.direction == Direction::In ? "input" : "output") +
                      " missing-instances=" + uncovered);
      }
      if (missing) model.unusedOutputs.push_back(p.getName().str());
    }
  }
  auto passthroughs = analyzeTopPassthroughs(*hierarchy, channels, renames, claimedTop, error);
  if (!passthroughs) return failure();
  // Reject uncovered scalar ports rather than silently leave unqueued data in
  // an otherwise complete transformed graph.
  for (auto [i, p] : llvm::enumerate(top.getPorts()))
    if (p.getName() != "hostClock" && p.getName() != "hostReset" && !isa<ClockType>(p.type) && !claimedTop.count(i))
      return reject("non-model scalar top data passthrough is unsupported: " + p.getName().str());
  std::map<std::string, std::string> hierarchicalRenames;
  for (auto attr : raw) {
    Annotation anno(attr);
    bool referencesData = false;
    std::string targetError;
    mapRetainedAnnotationTargets(attr, [&](StringAttr target) {
      auto spelling = target.getValue();
      referencesData |= renames.count(spelling.str()) != 0;
      auto prefix = "~" + circuit.getName().str() + "|";
      if (!spelling.starts_with(prefix) || !spelling.split('>').first.contains('/') ||
          spelling.split('>').second.empty()) return target;
      auto root = spelling.drop_front(prefix.size()).split('/').first;
      // SFC drops model-rooted DontTouches regardless of the selected leaf.
      if (anno.isClass(AnnotationClasses::DontTouch) &&
          llvm::any_of(models, [&](auto &model) { return model.op.getName() == root; }))
        return target;
      auto resolved = resolveHierarchicalReference(circuit, spelling, targetError);
      if (!resolved) return target;
      auto rename = renames.find(resolved->localTarget);
      if (rename == renames.end()) return target;
      referencesData = true;
      if (!anno.isClass(AnnotationClasses::DontTouch)) {
        targetError = "instance-qualified selected data target has no supported annotation transfer policy";
        return target;
      }
      std::string resolutionError;
      auto port = resolveAnnotationTarget(circuit, resolved->localTarget, resolutionError);
      if (!port || !port->port || *port->fieldID != 0) {
        targetError = "instance-qualified selected target does not resolve to the original ground port";
        return target;
      }
      auto payload = StringRef(rename->second).split('>').second;
      hierarchicalRenames.emplace(spelling.str(), resolved->hierarchy + ">" + payload.str());
      return target;
    });
    if (!targetError.empty()) return reject(targetError);
    if (referencesData && !anno.isClass(AnnotationClasses::ChannelPorts) &&
        !anno.isClass(AnnotationClasses::ChannelConnection) && !anno.isClass(AnnotationClasses::DontTouch) &&
        !anno.isClass(AnnotationClasses::CombinationalPath) && !anno.isClass(AnnotationClasses::InternalFpgaDebug) &&
        !anno.isClass(AnnotationClasses::ModelReadPort) &&
        !anno.isClass(AnnotationClasses::ModelWritePort) &&
        !anno.isClass(AnnotationClasses::ModelReadWritePort))
      return reject("data target has unsupported retained annotation metadata");
  }
  // Everything retained below is an owned name/type/selection snapshot. Drop
  // binding pointers and immutable lookup indices before the first mutation.
  BundleType clockType;
  for (const auto &p : plan->sinks)
    if (p.binding->globalName == clockGlobal) clockType = p.type;
  plan.reset();
  bindings.clear();
  connectionsByPort.clear();
  channels = {};
  hierarchy = nullptr;
  prepared.reset();
  SmallVector<FModuleOp> selectedModels; for (auto &m : models) selectedModels.push_back(m.op);
  if (failed(consumeFAMEModelDontTouches(circuit, selectedModels, error))) return failure();
  for (auto &m : models) if (failed(addFAMEHostControl(circuit, m.op, error))) return failure();
  SmallVector<FAMEHubClockControl, 0> hubControls;
  if (hub) {
    auto current = analyzeTopHierarchy(circuit, error); if (!current) return failure();
    ModelPortGroup clockGroup;
    bool foundClockGroup = false;
    for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
      Annotation anno(attr); if (!anno.isClass(AnnotationClasses::ChannelPorts)) continue;
      auto group = analyzeModelPortGroup(circuit, anno, error); if (!group) return failure();
      if (group->module == hub->op && group->name == clockLocal) { clockGroup = *group; foundClockGroup = true; }
    }
    if (!foundClockGroup) return reject("lost prepared hub clock group");
    SmallVector<unsigned> clockIndices;
    for (auto &domain : domains) {
      auto modelIndex = port(hub->op, domain.modelClockName), topIndex = port(top, domain.topClockName);
      if (!modelIndex || !topIndex) return reject("lost hub clock port");
      domain.modelPort = *modelIndex; domain.topPort = *topIndex; clockIndices.push_back(*modelIndex);
    }
    auto liveHub = instance(top, hubInstance);
    ModelChannelBinding clockBinding{clockGlobal, &clockGroup, liveHub, clockIndices};
    // The prepared clock port type is captured before rewriting host ports.
    if (!liveHub || !clockType) return reject("lost live hub clock instance/type");
    FAMETopChannelPort clockEndpoint{&clockBinding, clockTopPort, clockType};
    if (failed(rewriteFAMEHubClockChannel(*current, clockEndpoint, domains, true, error))) return failure();
    auto constructed = constructFAMEHubClockControls(circuit, hub->op, clockLocal, domains, error);
    if (!constructed) return failure();
    hubControls.assign(constructed->begin(), constructed->end());
  }
  SmallVector<FAMEModelControlPlan, 0> controls;
  for (auto &model : models) {
    FAMEModelControlPlan control; control.model = model.op;
    if (hub && model.op == hub->op) { control.clockChannel = clockLocal; control.clocks = hubControls; }
    else {
      auto clock = constructFAMEVirtualClockControls(circuit, model.op, model.targetClock, error);
      if (!clock) return failure(); control.clocks.push_back(*clock);
    }
    for (const auto &assignment : model.assignments)
      control.channels.push_back({assignment.localName, assignment.direction,
          assignment.hasClockDomain ? std::optional<std::string>(assignment.modelClockName) : std::nullopt});
    for (const auto &out : model.selection.outputs) control.dependencies.push_back(out.dependency);
    controls.push_back(std::move(control));
  }
  // Keep the clock constructor's schema-aware annotation transfers. Apply
  // captured data renames only after all shared instances have their new ABI.
  std::set<std::pair<std::string, std::string>> rewritten;
  for (const auto &endpoint : endpoints) {
    bool definition = rewritten.insert({endpoint.model, endpoint.local}).second;
    auto current = captureChannelRewriteConnections(top);
    auto model = find(circuit, endpoint.model); auto live = instance(top, endpoint.instance);
    SmallVector<unsigned> indices;
    // The definition can already have collapsed a grouped channel while this
    // independent instance still has its original leaf ports. Resolve physical
    // indices from that live instance rather than from the rewritten definition.
    for (const auto &old : endpoint.oldPorts)
      for (unsigned i = 0; live && i < live.getNumResults(); ++i)
        if (live.getPortNameStr(i) == old) indices.push_back(i);
    if (!live || indices.size() != endpoint.oldPorts.size()) return reject("lost data channel during shared-instance rewrite");
    ModelPortGroup group{endpoint.local, model, endpoint.direction, std::nullopt, indices};
    ModelChannelBinding binding{endpoint.global, &group, live, indices};
    FAMETopChannelPort channel{&binding, endpoint.topPort, endpoint.type};
    if (failed(endpoint.direction == Direction::In ? rewriteFAMEInputChannel(current, channel, error, definition)
                                                   : rewriteFAMEOutputChannel(current, channel, error, definition))) return failure();
  }
  rewriteTopPassthroughs(top, *passthroughs);
  SmallVector<Attribute> annotations;
  for (auto attr : circuit->getAttrOfType<ArrayAttr>("rawAnnotations")) {
    Annotation anno(attr);
    annotations.push_back(anno.isClass(AnnotationClasses::InternalFpgaDebug) ? attr :
        mapRetainedAnnotationTargets(attr, [&](StringAttr value) {
          auto local = renames.find(value.getValue().str());
          if (local != renames.end()) return StringAttr::get(circuit.getContext(), local->second);
          auto hierarchical = hierarchicalRenames.find(value.getValue().str());
          return hierarchical == hierarchicalRenames.end() ? value :
              StringAttr::get(circuit.getContext(), hierarchical->second);
        }));
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(circuit.getContext(), annotations));
  // Debug transfers validate and scan retained annotations for each renamed
  // port. With no debug annotations there is no transfer to perform.
  bool hasDebugTargets = llvm::any_of(annotations, [](Attribute attr) {
    return Annotation(attr).isClass(AnnotationClasses::InternalFpgaDebug);
  });
  if (hasDebugTargets)
    for (const auto &[oldTarget, payload] : renames)
      if (failed(transferFAMEPortDebugTargets(circuit, oldTarget, payload, error))) return failure();
  if (failed(rewriteFAMEModelControls(circuit, controls, error))) return failure();
  for (auto &model : models) {
    // SFC keeps unused clock outputs as local wires in every transformed
    // definition, including virtual-clock memory and combinational models.
    // All retained instances must reach the shared clock-free ABI together.
    SmallVector<StringRef> instanceNames;
    for (const auto &name : model.instances) instanceNames.push_back(name);
    if (failed(internalizeFAMEOutputClocks(top, model.op, instanceNames, error)))
      return failure();
    SmallVector<StringRef> unused; for (const auto &p : model.unusedOutputs) unused.push_back(p);
    if (failed(internalizeFAMEUnusedOutputs(circuit, model.op, unused, error)) ||
        failed(groupFAMEChannelPorts(top, model.op, model.instances.front(),
             hub && model.op == hub->op ? clockLocal + "_sink" : "", error))) return failure();
  }
  for (auto &model : models)
    for (auto p : model.op.getPorts())
      if (p.direction == Direction::In && isa<ClockType>(p.type) && p.getName() != "hostClock")
        return reject("unconsumed scalar model clock input: " + model.op.getName().str() + "." +
                      p.getName().str() + "; prove hub aliases before ChannelExcision");
  if (failed(removeFAMEStaleTopClocks(top, error))) return failure();
  for (const auto &model : models)
    for (const auto &[name, metadata] : model.instanceMetadata) {
      auto retained = instance(top, name);
      auto definition = model.op;
      if (!retained || retained.getModuleName() != definition.getName())
        return reject("lost retained model instance identity");
      for (auto attr : metadata) retained->setAttr(attr.getName(), attr.getValue());
    }
  // Thread complete FAME hardware while the canonical model/top channel ABI is
  // still present. SimWrapper queues and target-clock conversion must follow
  // grouping; doing this after transport would bank queue state and lose the
  // logical-to-physical model schedule.
  if (enableMultiThreading) {
    // SFC establishes residual fanout before grouping. These annotations name
    // unchanged logical top endpoints, never the shared physical model ports.
    if (failed(addRemainingFanoutAnnotations(circuit, error))) return failure();
    if (failed(multiThreadFAME5Models(circuit, result.threadedGroups, error,
                                    threadingClocks))) return failure();
    if (auto allocation = circuit->getAttrOfType<ArrayAttr>("goldengate.fame5.groups"))
      for (auto group : allocation)
        result.sharedThreads += cast<DictionaryAttr>(group).getAs<IntegerAttr>("threads").getInt();
  }
  if (withTransport && ((!enableMultiThreading && failed(addRemainingFanoutAnnotations(circuit, error))) ||
      failed(addFAMEBoundaryPipeChannels(circuit, error)) || failed(addFAMEPipeWrapper(circuit, error)) ||
      failed(addFAMEBoundaryReadyValidChannels(circuit, error)) || failed(addFAMEClockChannel(circuit, error)) ||
      failed(activateFAMEPipeWrapper(circuit, error)))) return failure();
  result.definitions = models.size(); result.clockDomains = domains.size(); result.dataEndpoints = endpoints.size();
  for (const auto &model : models) result.instances += model.instances.size();
  return success();
}
} // namespace
LogicalResult goldengate::rewriteFAMEModelGraph(CircuitOp circuit,
    FAMEModelGraphResult &result, std::string &error, bool withTransport,
    bool enableMultiThreading, FAME5ClockSemantics threadingClocks) {
  OwningOpRef<CircuitOp> staged(cast<CircuitOp>(circuit->clone()));
  FAMEModelGraphResult count;
  if (failed(foldHubClockAliases(*staged, error)) ||
      failed(rewrite(*staged, count, error, withTransport, enableMultiThreading, threadingClocks))) return failure();
  if (failed(verify(*staged))) { error = "FAME model graph produced invalid FIRRTL IR"; return failure(); }
  circuit->setAttrs((*staged)->getAttrs()); circuit.getBody().takeBody(staged->getBody()); result = count;
  return success();
}
