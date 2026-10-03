// See LICENSE for license details.
#include "goldengate/FindDefaultClocks.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <deque>

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::findDefaultClocks(CircuitOp circuit,
                                             std::string &error) {
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!top || !raw) {
    error = "FindDefaultClocks needs a top module and retained annotations";
    return failure();
  }

  std::string topPrefix = "~" + circuit.getName().str() + "|" +
                          top.getName().str() + ">";
  StringAttr referenceClockPort;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
      continue;
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") !=
                     goldengate::AnnotationClasses::TargetClockChannel)
      continue;
    auto sinks = annotation.getMember<ArrayAttr>("sinks");
    if (sinks && !sinks.empty()) {
      auto target = dyn_cast<StringAttr>(sinks[0]);
      if (target && target.getValue().starts_with(topPrefix)) {
        referenceClockPort = StringAttr::get(
            circuit.getContext(), target.getValue().drop_front(topPrefix.size()));
        break;
      }
    }
  }
  if (!referenceClockPort) {
    error = "no target-clock channel sink on the wrapper top";
    return failure();
  }

  DenseMap<Value, StringAttr> topPortNames;
  for (unsigned i = 0; i < top.getNumPorts(); ++i)
    topPortNames[top.getBodyBlock()->getArgument(i)] =
        StringAttr::get(circuit.getContext(), top.getPortName(i));
  DenseMap<StringAttr, InstanceOp> instances;
  for (auto instance : top.getOps<InstanceOp>())
    instances[StringAttr::get(circuit.getContext(), instance.getName())] =
        instance;
  auto instancePort = [](Value value) -> std::optional<std::pair<InstanceOp, unsigned>> {
    auto result = dyn_cast<OpResult>(value);
    if (!result)
      return std::nullopt;
    auto instance = dyn_cast<InstanceOp>(result.getOwner());
    if (!instance)
      return std::nullopt;
    return std::make_pair(instance, result.getResultNumber());
  };

  InstanceOp hub;
  DenseMap<Operation *, SmallVector<Operation *>> adjacency;
  DenseMap<Operation *, SmallVector<StringAttr>> directClocks;
  auto inspect = [&](Value dest, Value source) {
    auto lhs = instancePort(dest);
    auto rhs = instancePort(source);
    if (lhs && topPortNames.count(source) &&
        topPortNames.lookup(source) == referenceClockPort) {
      if (hub && hub != lhs->first)
        error = "target-clock channel sink drives multiple hub models";
      else
        hub = lhs->first;
    }
    if (!lhs || !rhs || !isa<ClockType>(dest.getType()) ||
        !isa<ClockType>(source.getType()))
      return;
    adjacency[lhs->first.getOperation()].push_back(rhs->first.getOperation());
    adjacency[rhs->first.getOperation()].push_back(lhs->first.getOperation());
  };
  top.walk([&](Operation *op) {
    if (auto connect = dyn_cast<ConnectOp>(op))
      inspect(connect.getDest(), connect.getSrc());
    else if (auto connect = dyn_cast<StrictConnectOp>(op))
      inspect(connect.getDest(), connect.getSrc());
  });
  if (!error.empty())
    return failure();
  if (!hub) {
    error = "target-clock channel sink is not connected to a hub model";
    return failure();
  }
  // A hub port is a separate clock graph node. This preserves distinct hub
  // clocks even when one hub instance drives several satellite domains.
  top.walk([&](Operation *op) {
    auto inspectClock = [&](Value dest, Value source) {
      auto lhs = instancePort(dest);
      auto rhs = instancePort(source);
      if (!lhs || !rhs || !isa<ClockType>(dest.getType()) ||
          !isa<ClockType>(source.getType()))
        return;
      auto add = [&](InstanceOp candidate, unsigned port, InstanceOp other) {
        if (candidate != hub || other == hub)
          return;
        std::string target = topPrefix + candidate.getName().str() + "." +
                             candidate.getPortNameStr(port).str();
        directClocks[other.getOperation()].push_back(
            StringAttr::get(circuit.getContext(), target));
      };
      add(lhs->first, lhs->second, rhs->first);
      add(rhs->first, rhs->second, lhs->first);
    };
    if (auto connect = dyn_cast<ConnectOp>(op))
      inspectClock(connect.getDest(), connect.getSrc());
    else if (auto connect = dyn_cast<StrictConnectOp>(op))
      inspectClock(connect.getDest(), connect.getSrc());
  });

  auto defaultClock = [&](InstanceOp satellite) -> StringAttr {
    std::deque<Operation *> queue{satellite.getOperation()};
    llvm::DenseSet<Operation *> visited;
    StringAttr found;
    while (!queue.empty()) {
      Operation *node = queue.front();
      queue.pop_front();
      if (!visited.insert(node).second)
        continue;
      for (StringAttr clock : directClocks[node]) {
        if (found && found != clock) {
          error = "satellite model has ambiguous default clocks: " +
                  satellite.getName().str();
          return {};
        }
        found = clock;
      }
      for (Operation *neighbor : adjacency[node])
        if (neighbor != hub.getOperation())
          queue.push_back(neighbor);
    }
    if (!found)
      error = "satellite model has no connected hub clock: " +
              satellite.getName().str();
    return found;
  };
  auto modelFor = [&](ArrayAttr endpoints) -> InstanceOp {
    if (!endpoints || endpoints.empty())
      return {};
    auto target = dyn_cast<StringAttr>(endpoints[0]);
    if (!target || !target.getValue().starts_with(topPrefix))
      return {};
    StringRef local = target.getValue().drop_front(topPrefix.size());
    auto pair = local.split('.');
    if (pair.second.empty())
      return {};
    return instances.lookup(StringAttr::get(circuit.getContext(), pair.first));
  };

  SmallVector<Attribute> updated;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection) &&
        !annotation.getMember<StringAttr>("clock")) {
      auto sources = annotation.getMember<ArrayAttr>("sources");
      auto sinks = annotation.getMember<ArrayAttr>("sinks");
      if (sources && sinks && !sources.empty() && !sinks.empty()) {
        InstanceOp source = modelFor(sources), sink = modelFor(sinks);
        if (!source || !sink) {
          error = "unclocked inter-model channel has unresolved endpoints";
          return failure();
        }
        InstanceOp satellite = sink != hub ? sink : source != hub ? source
                                                              : InstanceOp();
        if (satellite) {
          StringAttr clock = defaultClock(satellite);
          if (!clock)
            return failure();
          annotation.setMember("clock", clock);
        }
      }
    }
    updated.push_back(annotation.getAttr());
  }

  // Hub self-loops inherit a clock from another channel with identical source
  // ports. SFC performs this as a second annotation pass.
  DenseMap<ArrayAttr, StringAttr> sourceClocks;
  for (Attribute attr : updated) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection)) {
      auto sources = annotation.getMember<ArrayAttr>("sources");
      auto clock = annotation.getMember<StringAttr>("clock");
      if (sources && clock)
        sourceClocks[sources] = clock;
    }
  }
  for (Attribute &attr : updated) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::ChannelConnection) ||
        annotation.getMember<StringAttr>("clock"))
      continue;
    auto sources = annotation.getMember<ArrayAttr>("sources");
    auto sinks = annotation.getMember<ArrayAttr>("sinks");
    if (!sources || !sinks)
      continue;
    auto found = sourceClocks.find(sources);
    if (found == sourceClocks.end()) {
      error = "hub self-loop has no clocked channel with matching source";
      return failure();
    }
    annotation.setMember("clock", found->second);
    attr = annotation.getAttr();
  }
  circuit->setAttr("rawAnnotations",
                   ArrayAttr::get(circuit.getContext(), updated));
  return success();
}
