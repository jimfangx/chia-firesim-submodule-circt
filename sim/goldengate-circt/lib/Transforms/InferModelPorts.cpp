// See LICENSE for license details.
#include "goldengate/InferModelPorts.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/ChannelAnalysis.h"
#include "goldengate/HierarchyAnalysis.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "llvm/ADT/DenseSet.h"
#include <vector>

using namespace circt::firrtl;
using namespace mlir;

namespace {
struct PortGroup {
  FModuleLike module;
  std::string name;
  std::optional<unsigned> clock;
  std::vector<unsigned> ports;
};

const goldengate::TopPortConnection *
lookupConnection(const goldengate::TopHierarchy &hierarchy, unsigned topPort) {
  auto found = llvm::find_if(hierarchy.connections,
                             [&](const goldengate::TopPortConnection &entry) {
                               return entry.topPort == topPort;
                             });
  return found == hierarchy.connections.end() ? nullptr : &*found;
}
} // namespace

LogicalResult goldengate::inferModelPorts(CircuitOp circuit,
                                          std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "InferModelPorts needs retained FAME annotations";
    return failure();
  }
  auto hierarchy = analyzeTopHierarchy(circuit, error);
  if (!hierarchy)
    return failure();

  llvm::DenseSet<Operation *> transformed;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::FAMETransform))
      continue;
    auto spelling = annotation.getMember<StringAttr>("target");
    auto target = spelling
                      ? resolveAnnotationTarget(circuit, spelling.getValue(),
                                                error)
                      : std::nullopt;
    if (!target || target->port || !isa<FModuleOp>(target->module)) {
      if (error.empty())
        error = "FAME transform target is not an internal module";
      return failure();
    }
    transformed.insert(target->module.getOperation());
  }

  std::vector<PortGroup> groups;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto channel = analyzeChannelConnection(circuit, annotation, error);
    if (!channel)
      return failure();

    // Each side contributes a local channel group to every transformed model
    // instance connected to that side. Keep the annotation's endpoint order:
    // it determines the fields of an aggregated decoupled payload.
    for (const auto &endpoints : {llvm::ArrayRef<GGTarget>(channel->sources),
                                  llvm::ArrayRef<GGTarget>(channel->sinks)}) {
      struct InstanceGroup {
        InstanceOp instance;
        FModuleLike module;
        std::vector<unsigned> ports;
      };
      std::vector<InstanceGroup> instances;
      for (const auto &endpoint : endpoints) {
        if (endpoint.module != hierarchy->top || !endpoint.port) {
          error = "channel endpoint is not an excised top port: " +
                  channel->name;
          return failure();
        }
        auto *connection = lookupConnection(*hierarchy, *endpoint.port);
        if (!connection)
          continue; // Bridge-facing endpoint or top-level loopback.
        auto connectedInstance = connection->instance;
        FModuleLike module;
        for (Operation &op : circuit.getBodyBlock()->getOperations())
          if (auto candidate = dyn_cast<FModuleLike>(&op);
              candidate && candidate.getModuleName() ==
                               connectedInstance.getModuleName())
            module = candidate;
        if (!module || !transformed.contains(module.getOperation()))
          continue;
        auto found = llvm::find_if(instances, [&](const InstanceGroup &entry) {
          return entry.instance == connection->instance;
        });
        if (found == instances.end())
          instances.push_back({connection->instance, module,
                               {connection->instancePort}});
        else
          found->ports.push_back(connection->instancePort);
      }

      for (auto &instance : instances) {
        std::optional<unsigned> clock;
        if (channel->clock && channel->clock->module == hierarchy->top &&
            channel->clock->port) {
          auto *connection = lookupConnection(*hierarchy,
                                              *channel->clock->port);
          if (connection) {
            auto clockInstance = connection->instance;
            if (clockInstance.getModuleName() ==
                instance.module.getModuleName())
              clock = connection->instancePort;
          }
        }
        std::string name = instance.ports.size() == 1
                               ? instance.module.getPortName(instance.ports[0]).str()
                               : channel->name;
        auto samePorts = [&](const PortGroup &group) {
          return group.module == instance.module && group.ports == instance.ports &&
                 group.clock == clock;
        };
        if (llvm::any_of(groups, samePorts))
          continue;
        for (const auto &group : groups) {
          if (group.module != instance.module)
            continue;
          for (unsigned port : instance.ports)
            if (llvm::is_contained(group.ports, port)) {
              error = "channel definitions partially overlap model ports: " +
                      channel->name;
              return failure();
            }
        }
        groups.push_back({instance.module, std::move(name), clock,
                          std::move(instance.ports)});
      }
    }
  }

  auto *context = circuit.getContext();
  auto key = [&](llvm::StringRef text) { return StringAttr::get(context, text); };
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  for (const auto &group : groups) {
    auto model = group.module;
    std::string prefix = "~" + circuit.getName().str() + "|" +
                         model.getModuleName().str() + ">";
    SmallVector<Attribute> ports;
    for (unsigned port : group.ports)
      ports.push_back(key(prefix + model.getPortName(port).str()));
    NamedAttrList members;
    members.set("class", key(AnnotationClasses::ChannelPorts));
    members.set("localName", key(group.name));
    members.set("ports", ArrayAttr::get(context, ports));
    if (group.clock)
      members.set("clockPort",
                  key(prefix + model.getPortName(*group.clock).str()));
    annotations.push_back(DictionaryAttr::get(context, members));

    // SFC protects both the clock and data ports when it creates a channel
    // port annotation. Keep the same target identities available to later
    // CIRCT transformations even though the FAME-only dump omits DontTouch.
    auto protect = [&](Attribute target) {
      NamedAttrList dontTouch;
      dontTouch.set("class", key(AnnotationClasses::DontTouch));
      dontTouch.set("target", target);
      annotations.push_back(DictionaryAttr::get(context, dontTouch));
    };
    if (group.clock)
      protect(key(prefix + model.getPortName(*group.clock).str()));
    for (Attribute port : ports)
      protect(port);
  }
  circuit->setAttr("rawAnnotations", ArrayAttr::get(context, annotations));
  return success();
}
