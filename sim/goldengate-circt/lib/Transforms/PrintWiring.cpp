// See LICENSE for license details.
#include "goldengate/PrintWiring.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Support/InstanceGraph.h"
#include "circt/Support/Namespace.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using namespace mlir;
using namespace circt::firrtl;

namespace {
constexpr llvm::StringLiteral prefix = "synthesizedPrintf_";
// FIRRTL StringLit.serialize uses escapeJava, without surrounding quotes.
// Escape UTF-16 code units here; the annotation JSON exporter escapes this
// serialized format again when writing PrintBridgeParameters.
std::optional<std::string> serializePrintFormat(StringRef format) {
  SmallVector<llvm::UTF16> units;
  if (!llvm::convertUTF8ToUTF16String(format, units)) return std::nullopt;
  std::string result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (auto unit : units) {
    switch (unit) {
    case '\b': result += "\\b"; break;
    case '\t': result += "\\t"; break;
    case '\n': result += "\\n"; break;
    case '\f': result += "\\f"; break;
    case '\r': result += "\\r"; break;
    case '"': result += "\\\""; break;
    case '\\': result += "\\\\"; break;
    default:
      if (unit < 0x20 || unit > 0x7f) {
        result += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
          result += hex[(unit >> shift) & 15];
      } else result += char(unit);
    }
  }
  return result;
}
struct Route {
  unsigned stubIndex, port;
  SmallVector<Operation *> path;
  StringAttr name;
  // For a descendant source, read this port of path.front().
  unsigned childPort = 0;
};
struct ModuleRoutes {
  FModuleOp module;
  unsigned oldPorts;
  SmallVector<Route> routes;
  SmallVector<std::pair<unsigned, PortInfo>> added;
};
} // namespace

LogicalResult goldengate::wirePrintStubsToTop(
    CircuitOp circuit, ArrayRef<PrintStub> stubs,
    SmallVectorImpl<WiredPrint> &outputs, std::string &error) {
  // All planning uses native instance graph records and operation identities.
  // Destroy the graph before replacing any of its InstanceOps.
  std::map<Operation *, ModuleRoutes> modules;
  SmallVector<Operation *> order;
  SmallVector<std::pair<InstanceOp, Operation *>> uses;
  FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *topNode = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!topNode || !topNode->noUses() ||
        !(top = dyn_cast<FModuleOp>(topNode->getModule().getOperation()))) {
      error = "printf top wiring needs an uninstantiated internal circuit top";
      return failure();
    }
    llvm::DenseMap<Operation *, SmallVector<unsigned>> local;
    llvm::DenseSet<Operation *> selected;
    for (auto [i, stub] : llvm::enumerate(stubs)) {
      auto bundle = stub.bundle;
      auto module = bundle ? bundle->getParentOfType<FModuleOp>() : FModuleOp();
      auto type = bundle ? dyn_cast<BundleType>(bundle.getResult().getType()) : BundleType();
      if (!module || module->getParentOp() != circuit.getOperation() ||
          bundle->getBlock() != module.getBodyBlock() || !type || !type.isPassive() ||
          !selected.insert(bundle.getOperation()).second) {
        error = "printf top wiring needs distinct module-scope passive bundle sources";
        return failure();
      }
      local[module.getOperation()].push_back(i);
    }
    llvm::DenseSet<Operation *> active;
    std::function<LogicalResult(circt::igraph::InstanceGraphNode *)> plan =
        [&](circt::igraph::InstanceGraphNode *node) -> LogicalResult {
      auto module = dyn_cast<FModuleOp>(node->getModule().getOperation());
      if (!module) return success(); // Extmodules have no local printf sources.
      auto *key = module.getOperation();
      if (modules.count(key)) return success();
      if (!active.insert(key).second) {
        error = "recursive module hierarchy in printf top wiring";
        return failure();
      }
      ModuleRoutes info{module, unsigned(module.getNumPorts()), {}, {}};
      for (unsigned index : local[key])
        info.routes.push_back({index, 0, {}, {}, 0});
      for (auto *record : *node) {
        if (failed(plan(record->getTarget()))) return failure();
        auto child = modules.find(record->getTarget()->getModule().getOperation());
        if (child == modules.end() || child->second.routes.empty()) continue;
        auto instance = record->getInstance<InstanceOp>();
        if (!instance || instance->getBlock() != module.getBodyBlock()) {
          error = "printf top wiring requires module-scope FIRRTL instances";
          return failure();
        }
        for (auto route : child->second.routes) {
          route.childPort = route.port;
          route.path.insert(route.path.begin(), instance.getOperation());
          info.routes.push_back(std::move(route));
        }
      }
      circt::Namespace names;
      for (auto name : module.getPortNamesAttr())
        names.newName(cast<StringAttr>(name).getValue());
      module.walk([&](Operation *op) {
        if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
      });
      for (auto [i, route] : llvm::enumerate(info.routes)) {
        std::string suggested = prefix.str();
        for (auto *instance : route.path)
          suggested += cast<InstanceOp>(instance).getName().str() + "_";
        auto bundle = stubs[route.stubIndex].bundle;
        suggested += bundle.getName().str();
        route.port = info.oldPorts + i;
        route.name = StringAttr::get(circuit.getContext(), names.newName(suggested));
        info.added.push_back({info.oldPorts,
            PortInfo(route.name, bundle.getResult().getType(), Direction::Out)});
      }
      active.erase(key);
      modules.emplace(key, std::move(info));
      order.push_back(key);
      return success();
    };
    // Include unused parents so their instances also receive the expanded
    // child signature. Their existing circuitry and metadata remain valid.
    for (auto module : circuit.getOps<FModuleOp>())
      if (failed(plan(graph.lookup(module)))) return failure();
    llvm::DenseSet<unsigned> reachable;
    for (auto &route : modules.at(top.getOperation()).routes)
      reachable.insert(route.stubIndex);
    if (reachable.size() != stubs.size()) {
      error = "printf bundle source has no instance path from the circuit top";
      return failure();
    }
    for (auto &[key, info] : modules) {
      if (info.routes.empty()) continue;
      for (auto *record : graph.lookup(info.module)->uses()) {
        auto instance = record->getInstance<InstanceOp>();
        auto parent = instance ? instance->getParentOfType<FModuleOp>() : FModuleOp();
        if (!instance || !parent || instance.getNumResults() != info.oldPorts ||
            instance->getBlock() != parent.getBodyBlock()) {
          error = "printf source module has an incompatible instance use";
          return failure();
        }
        uses.push_back({instance, key});
      }
    }
  }

  for (auto *key : order) {
    auto &info = modules.at(key);
    if (!info.added.empty()) info.module.insertPorts(info.added);
  }
  llvm::DenseMap<Operation *, InstanceOp> replacements;
  for (auto [instance, child] : uses) {
    auto &info = modules.at(child);
    auto replacement = instance.cloneAndInsertPorts(info.added);
    // CIRCT rebuilds the signature and known instance attributes. Retain any
    // additional metadata without overwriting the expanded port attributes.
    for (auto attr : instance->getAttrs())
      if (!replacement->hasAttr(attr.getName()))
        replacement->setAttr(attr.getName(), attr.getValue());
    for (unsigned i = 0; i < info.oldPorts; ++i)
      instance.getResult(i).replaceAllUsesWith(replacement.getResult(i));
    replacements[instance.getOperation()] = replacement;
    instance.erase();
  }
  OpBuilder b(circuit.getContext());
  for (auto *key : order) {
    auto &info = modules.at(key);
    b.setInsertionPointToEnd(info.module.getBodyBlock());
    for (auto &route : info.routes) {
      auto bundle = stubs[route.stubIndex].bundle;
      Value driver = bundle.getResult();
      if (!route.path.empty())
        driver = replacements.lookup(route.path.front()).getResult(route.childPort);
      b.create<StrictConnectOp>(bundle.getLoc(),
          info.module.getBodyBlock()->getArgument(route.port), driver);
    }
  }
  SmallVector<WiredPrint> result;
  for (auto &route : modules.at(top.getOperation()).routes) {
    auto bundle = stubs[route.stubIndex].bundle;
    std::string absolute = "~" + circuit.getName().str() + "|" + top.getName().str();
    // Returned paths contain live rebuilt instances, unlike the planning keys.
    SmallVector<InstanceOp> path;
    for (auto *old : route.path) {
      auto instance = replacements.lookup(old);
      path.push_back(instance);
      absolute += "/" + instance.getName().str() + ":" + instance.getModuleName().str();
    }
    absolute += ">" + bundle.getName().str();
    result.push_back({route.stubIndex, std::move(path),
        top.getBodyBlock()->getArgument(route.port), absolute,
        "~" + circuit.getName().str() + "|" + top.getName().str() + ">" + route.name.getValue().str()});
  }
  outputs.append(result.begin(), result.end());
  return success();
}

static LogicalResult synthesizePrintChannelsImpl(
    CircuitOp circuit, ArrayRef<goldengate::PrintStub> stubs,
    std::string &error, bool complete) {
  using goldengate::AnnotationClasses;
  using goldengate::resolveAnnotationTarget;
  FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *node = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!node || !node->noUses() ||
        !(top = dyn_cast<FModuleOp>(node->getModule().getOperation()))) {
      error = "printf channels need an uninstantiated internal circuit top";
      return failure();
    }
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) { error = "printf channels need retained annotations"; return failure(); }
  OpBuilder b(circuit.getContext());
  std::map<std::string, BundleType> stubTypes;
  std::map<std::string, std::string> formats;
  for (auto stub : stubs) {
    auto type = stub.bundle ? dyn_cast<BundleType>(stub.bundle.getResult().getType()) : BundleType();
    auto owner = stub.bundle ? stub.bundle->getParentOfType<FModuleOp>() : FModuleOp();
    if (!type || !owner || owner->getParentOp() != circuit.getOperation() ||
        stub.target != "~" + circuit.getName().str() + "|" + owner.getName().str() +
            ">" + stub.bundle.getName().str() ||
        !stubTypes.emplace(stub.target, type).second) {
      error = "printf channels need distinct native bundle sources";
      return failure();
    }
    if (complete) {
      auto print = stub.print;
      if (!print || print->getParentOfType<FModuleOp>() != owner ||
          print->getBlock() != owner.getBodyBlock() ||
          type.getElements().size() != print.getSubstitutions().size() + 1) {
        error = "printf bridge constructor needs the native source printf";
        return failure();
      }
      for (auto [i, arg] : llvm::enumerate(print.getSubstitutions())) {
        auto field = type.getElements()[i + 1];
        if (field.name.getValue() != "args_" + std::to_string(i) ||
            field.type != arg.getType()) {
          error = "printf bridge fields do not match native printf operands";
          return failure();
        }
      }
      auto format = serializePrintFormat(print.getFormatString());
      if (!format) {
        error = "printf bridge format must be valid UTF-8";
        return failure();
      }
      formats.emplace(stub.target, std::move(*format));
    }
  }
  // Domain ordering matches Scala's sortBy(sinkClockPort.ref). Completed
  // wiring groups replicas of each pathless source before domain grouping,
  // as BridgeTopWiring.localToAbsSource does. Source groups follow native
  // module/statement order; Scala's hash-map order between groups may differ.
  // Payload packing and decoder offsets both consume this printPorts sequence.
  struct Domain {
    SmallVector<Attribute> channels, printPorts;
    std::string resetName;
  };
  std::map<std::string, Domain> domains;
  std::set<std::string> channelNames, sources, sinks;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (anno.isClass(AnnotationClasses::ChannelConnection)) {
      auto name = anno.getMember<StringAttr>("globalName");
      if (!name || !channelNames.insert(name.getValue().str()).second) {
        error = "printf channels found malformed or duplicate existing channel names";
        return failure();
      }
    }
  }
  auto channel = [&](StringRef name, StringRef clock, StringRef source) {
    return b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::ChannelConnection)),
        b.getNamedAttr("globalName", b.getStringAttr(name)),
        b.getNamedAttr("channelInfo", b.getDictionaryAttr({
            b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::PipeChannel)),
            b.getNamedAttr("latency", b.getI64IntegerAttr(0))})),
        b.getNamedAttr("clock", b.getStringAttr(clock)),
        b.getNamedAttr("sources", b.getArrayAttr({b.getStringAttr(source)}))});
  };
  std::string topPrefix = "~" + circuit.getName().str() + "|" + top.getName().str() + ">";
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::BridgeTopWiringOutput)) continue;
    auto source = anno.getMember<StringAttr>("pathlessSource");
    auto sink = anno.getMember<StringAttr>("topSink");
    auto clock = anno.getMember<StringAttr>("sinkClockPort");
    auto found = source ? stubTypes.find(source.getValue().str()) : stubTypes.end();
    auto port = sink ? resolveAnnotationTarget(circuit, sink.getValue(), error) : std::nullopt;
    auto clk = clock ? resolveAnnotationTarget(circuit, clock.getValue(), error) : std::nullopt;
    if (found == stubTypes.end() || !port || !clk || !port->port || !clk->port ||
        port->module.getOperation() != top.getOperation() ||
        clk->module.getOperation() != top.getOperation() ||
        port->fieldID.value_or(0) != 0 || clk->fieldID.value_or(0) != 0 ||
        top.getPortDirection(*port->port) != Direction::Out ||
        top.getPortDirection(*clk->port) != Direction::Out ||
        top.getPortType(*port->port) != found->second ||
        !isa<ClockType>(top.getPortType(*clk->port)) ||
        !sinks.insert(sink.getValue().str()).second) {
      error = "printf channels need distinct top bundle outputs and an output Clock";
      return failure();
    }
    sources.insert(found->first);
    auto portName = top.getPortName(*port->port).str();
    auto clockName = top.getPortName(*clk->port).str();
    auto &domain = domains[clockName];
    SmallVector<Attribute> fields;
    if (found->second.getElements().empty()) {
      error = "printf bundle has no fields"; return failure();
    }
    for (auto [i, field] : llvm::enumerate(found->second.getElements())) {
      auto intType = dyn_cast<IntType>(field.type);
      if (field.isFlip || !intType || intType.getWidthOrSentinel() < 0 ||
          (i == 0 && (field.name.getValue() != "enable" ||
                     !isa<UIntType>(field.type) || intType.getWidthOrSentinel() != 1))) {
        error = "printf channels need passive, sized integer fields and a boolean enable";
        return failure();
      }
      auto name = portName + "_" + field.name.getValue().str();
      if (!channelNames.insert(name).second) {
        error = "printf channel name collides with an existing channel: " + name;
        return failure();
      }
      domain.channels.push_back(channel(name, topPrefix + clockName,
          topPrefix + portName + "." + field.name.getValue().str()));
      if (complete) {
        auto type = (isa<UIntType>(field.type) ? "UInt<" : "SInt<") +
            std::to_string(intType.getWidthOrSentinel()) + ">";
        // json4s serializes a (String, String) tuple as a singleton object.
        fields.push_back(b.getDictionaryAttr({b.getNamedAttr(
            field.name.getValue(), b.getStringAttr(type))}));
      }
    }
    if (complete)
      domain.printPorts.push_back(b.getDictionaryAttr({
          b.getNamedAttr("name", b.getStringAttr(portName)),
          b.getNamedAttr("ports", b.getArrayAttr(fields)),
          b.getNamedAttr("format", b.getStringAttr(formats.at(found->first)))}));
  }
  if (sources.size() != stubs.size()) {
    error = "printf channels are missing completed source bindings";
    return failure();
  }
  circt::Namespace names;
  for (auto name : top.getPortNamesAttr()) names.newName(cast<StringAttr>(name).getValue());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
  });
  SmallVector<std::pair<unsigned, PortInfo>> added;
  SmallVector<Attribute> annotations;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (complete && (anno.isClass(AnnotationClasses::BridgeTopWiringOutput) ||
                     anno.isClass(AnnotationClasses::SynthPrintf))) continue;
    annotations.push_back(attr);
  }
  unsigned oldPorts = top.getNumPorts();
  for (auto &[clockName, domain] : domains) {
    domain.resetName = names.newName(clockName + "_globalReset");
    if (!channelNames.insert(domain.resetName).second) {
      error = "printf reset channel name collides with an existing channel";
      return failure();
    }
    added.push_back({oldPorts, PortInfo(b.getStringAttr(domain.resetName),
        UIntType::get(circuit.getContext(), 1), Direction::Out)});
    auto target = topPrefix + domain.resetName;
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::GlobalResetSink)),
        b.getNamedAttr("target", b.getStringAttr(target))}));
    if (complete) {
      NamedAttrList mapping;
      mapping.set(domain.resetName, b.getStringAttr(domain.resetName));
      for (auto attr : domain.channels) {
        auto name = Annotation(attr).getMember<StringAttr>("globalName");
        mapping.set(name.getValue(), name);
      }
      annotations.push_back(b.getDictionaryAttr({
          b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeIO)),
          b.getNamedAttr("target", b.getStringAttr(topPrefix + "synthesizedPrintf")),
          b.getNamedAttr("channelMapping", mapping.getDictionary(circuit.getContext())),
          b.getNamedAttr("widgetClass", b.getStringAttr(AnnotationClasses::PrintBridgeModule)),
          b.getNamedAttr("widgetConstructorKey", b.getDictionaryAttr({
              b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::PrintBridgeParameters)),
              b.getNamedAttr("resetPortName", b.getStringAttr(domain.resetName)),
              b.getNamedAttr("printPorts", b.getArrayAttr(domain.printPorts))}))}));
    }
    annotations.push_back(channel(domain.resetName, topPrefix + clockName, target));
    annotations.append(domain.channels.begin(), domain.channels.end());
  }
  if (!added.empty()) top.insertPorts(added);
  b.setInsertionPointToEnd(top.getBodyBlock());
  for (unsigned i = 0; i < added.size(); ++i) {
    auto zero = b.create<ConstantOp>(top.getLoc(), UIntType::get(circuit.getContext(), 1),
                                   llvm::APInt(1, 0));
    b.create<StrictConnectOp>(top.getLoc(), top.getBodyBlock()->getArgument(oldPorts + i), zero);
  }
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}

LogicalResult goldengate::synthesizePrintChannels(
    CircuitOp circuit, ArrayRef<PrintStub> stubs, std::string &error) {
  return synthesizePrintChannelsImpl(circuit, stubs, error, false);
}

LogicalResult goldengate::completePrintSynthesis(
    CircuitOp circuit, ArrayRef<PrintStub> stubs, std::string &error) {
  return synthesizePrintChannelsImpl(circuit, stubs, error, true);
}

LogicalResult goldengate::completePrintClockWiring(
    CircuitOp circuit, ArrayRef<PrintStub> stubs, ArrayRef<WiredPrint> routes,
    std::string &error) {
  FModuleOp top;
  {
    circt::igraph::InstanceGraph graph(circuit);
    auto *node = graph.lookup(StringAttr::get(circuit.getContext(), circuit.getName()));
    if (!node || !node->noUses() ||
        !(top = dyn_cast<FModuleOp>(node->getModule().getOperation()))) {
      error = "printf clock wiring needs an uninstantiated internal circuit top";
      return failure();
    }
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "printf clock wiring needs retained annotations";
    return failure();
  }
  std::map<std::string, std::string> pending;
  for (auto &stub : stubs) {
    auto bundle = stub.bundle;
    auto module = bundle ? bundle->getParentOfType<FModuleOp>() : FModuleOp();
    if (!module || module->getParentOp() != circuit.getOperation() ||
        bundle->getBlock() != module.getBodyBlock() ||
        stub.target != "~" + circuit.getName().str() + "|" +
            module.getName().str() + ">" + bundle.getName().str() ||
        !pending.emplace(stub.target, stub.clockTarget).second) {
      error = "printf clock wiring needs distinct native bundle targets";
      return failure();
    }
  }
  llvm::DenseSet<Attribute> consumed;
  std::set<std::string> annotated;
  for (auto attr : raw) {
    Annotation anno(attr);
    if (!anno.isClass(AnnotationClasses::BridgeTopWiring)) continue;
    auto target = anno.getMember<StringAttr>("target");
    auto clock = anno.getMember<StringAttr>("clock");
    auto found = target ? pending.find(target.getValue().str()) : pending.end();
    if (found == pending.end() || !clock || clock.getValue() != found->second) {
      error = "printf clock wiring has an unsupported pending BridgeTopWiring annotation";
      return failure();
    }
    annotated.insert(found->first);
    consumed.insert(attr);
  }
  if (annotated.size() != stubs.size()) {
    error = "printf clock wiring is missing pending BridgeTopWiring annotations";
    return failure();
  }
  llvm::DenseSet<Value> sinks;
  llvm::DenseSet<unsigned> routed;
  for (auto &route : routes) {
    auto port = dyn_cast_or_null<BlockArgument>(route.topPort);
    auto bundle = route.stubIndex < stubs.size() ? stubs[route.stubIndex].bundle : WireOp();
    if (route.stubIndex >= stubs.size() || !port ||
        port.getOwner() != top.getBodyBlock() ||
        top.getPortDirection(port.getArgNumber()) != Direction::Out ||
        port.getType() != bundle.getResult().getType() ||
        !sinks.insert(port).second) {
      error = "printf clock wiring needs distinct native top bundle outputs";
      return failure();
    }
    routed.insert(route.stubIndex);
  }
  if (routed.size() != stubs.size()) {
    error = "printf clock wiring has an unrouted bundle source";
    return failure();
  }
  SmallVector<PrintClockSource> sources;
  if (failed(analyzePrintClockSources(circuit, stubs, routes, sources, error)))
    return failure();

  circt::Namespace names;
  for (auto name : top.getPortNamesAttr())
    names.newName(cast<StringAttr>(name).getValue());
  top.walk([&](Operation *op) {
    if (auto name = op->getAttrOfType<StringAttr>("name")) names.newName(name.getValue());
  });
  OpBuilder b(circuit.getContext());
  llvm::DenseMap<Value, unsigned> clockPorts;
  SmallVector<std::pair<unsigned, PortInfo>> added;
  SmallVector<Value> drivers;
  unsigned oldPorts = top.getNumPorts();
  for (auto &source : sources) {
    if (clockPorts.count(source.source)) continue;
    auto port = cast<BlockArgument>(source.source);
    auto name = names.newName(prefix.str() + top.getPortName(port.getArgNumber()).str());
    clockPorts[source.source] = oldPorts + added.size();
    added.push_back({oldPorts, PortInfo(b.getStringAttr(name),
        ClockType::get(circuit.getContext()), Direction::Out)});
    drivers.push_back(source.source);
  }
  // Serialize identities from the native bundle, instance path and port values.
  // Do not use a dotted path or stale textual target to decide connectivity.
  std::string topPrefix = "~" + circuit.getName().str() + "|" + top.getName().str();
  SmallVector<Attribute> annotations;
  // BridgeTopWiring groups TopWiring mappings by local source, then expands
  // that source's absolute instances. A hierarchy walk interleaves distinct
  // printf records when a module has multiple prints and multiple instances.
  // Group using the native source identity, retaining instance traversal
  // within each group. Module/statement order makes inter-group order stable
  // without importing Scala's implementation-specific hash-map iteration.
  SmallVector<SmallVector<unsigned>> sourceGroups(stubs.size());
  for (auto [i, source] : llvm::enumerate(sources))
    sourceGroups[routes[source.routeIndex].stubIndex].push_back(i);
  for (auto &group : sourceGroups) for (auto i : group) {
    auto &source = sources[i];
    auto &route = routes[source.routeIndex];
    auto stub = stubs[route.stubIndex];
    std::string absolute = topPrefix;
    for (auto instance : route.instancePath)
      absolute += "/" + instance.getName().str() + ":" + instance.getModuleName().str();
    absolute += ">" + stub.bundle.getName().str();
    auto sink = cast<BlockArgument>(route.topPort);
    auto clockIndex = clockPorts.lookup(source.source) - oldPorts;
    annotations.push_back(b.getDictionaryAttr({
        b.getNamedAttr("class", b.getStringAttr(AnnotationClasses::BridgeTopWiringOutput)),
        b.getNamedAttr("pathlessSource", b.getStringAttr(stub.target)),
        b.getNamedAttr("absoluteSource", b.getStringAttr(absolute)),
        b.getNamedAttr("topSink", b.getStringAttr(topPrefix + ">" + top.getPortName(sink.getArgNumber()).str())),
        b.getNamedAttr("srcClockPort", b.getStringAttr(source.sourceTarget)),
        b.getNamedAttr("sinkClockPort", b.getStringAttr(topPrefix + ">" + added[clockIndex].second.name.getValue().str()))}));
  }
  for (auto attr : raw)
    if (!consumed.contains(attr)) annotations.push_back(attr);
  if (!added.empty()) top.insertPorts(added);
  b.setInsertionPointToEnd(top.getBodyBlock());
  for (auto [i, driver] : llvm::enumerate(drivers))
    b.create<StrictConnectOp>(top.getLoc(),
        top.getBodyBlock()->getArgument(oldPorts + i), driver);
  circuit->setAttr("rawAnnotations", b.getArrayAttr(annotations));
  return success();
}
