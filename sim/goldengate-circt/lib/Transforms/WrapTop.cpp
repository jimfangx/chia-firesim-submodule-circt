// See LICENSE for license details.
#include "goldengate/WrapTop.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/StringRef.h"
#include <functional>
#include <set>

using namespace circt::firrtl;
using namespace mlir;

namespace {
constexpr llvm::StringLiteral wrapperName = "FAMETop";

// Scala WrapTop explicitly renames channel endpoints to the wrapper ports.
// MemModel's component keeps the same module and reference, but its circuit
// changes with the wrapper. BridgeIO retains its serialized target here.
Attribute retarget(Attribute value, llvm::StringRef oldName,
                   bool channelEndpoint, MLIRContext *context) {
  if (auto text = dyn_cast<StringAttr>(value)) {
    llvm::StringRef spelling = text.getValue();
    std::string oldCircuit = ("~" + oldName).str();
    if (!spelling.consume_front(oldCircuit) ||
        (!spelling.empty() && spelling.front() != '|'))
      return value;
    std::string suffix = spelling.str();
    if (channelEndpoint) {
      std::string oldModule = ("|" + oldName + ">").str();
      if (llvm::StringRef(suffix).consume_front(oldModule)) {
        llvm::StringRef component(suffix);
        component = component.drop_front(oldModule.size());
        std::string flattened;
        for (char character : component) {
          if (character == '.' || character == '[')
            flattened += '_';
          else if (character != ']')
            flattened += character;
        }
        suffix = "|FAMETop>" + flattened;
      }
    }
    return StringAttr::get(context, "~FAMETop" + suffix);
  }
  if (auto array = dyn_cast<ArrayAttr>(value)) {
    SmallVector<Attribute> members;
    for (Attribute member : array)
      members.push_back(retarget(member, oldName, channelEndpoint, context));
    return ArrayAttr::get(context, members);
  }
  if (auto dict = dyn_cast<DictionaryAttr>(value)) {
    SmallVector<NamedAttribute> members;
    for (NamedAttribute member : dict)
      members.push_back(NamedAttribute(
          member.getName(),
          retarget(member.getValue(), oldName, channelEndpoint, context)));
    return DictionaryAttr::get(context, members);
  }
  return value;
}
} // namespace

LogicalResult goldengate::wrapTop(CircuitOp circuit, std::string &error) {
  auto *context = circuit.getContext();
  std::string oldName = circuit.getName().str();
  if (oldName == wrapperName) {
    error = "circuit is already wrapped";
    return failure();
  }
  FModuleOp oldTop;
  for (Operation &operation : circuit.getBodyBlock()->getOperations()) {
    auto module = dyn_cast<FModuleLike>(&operation);
    if (!module)
      continue;
    if (module.getModuleName() == wrapperName) {
      error = "FAMETop module already exists";
      return failure();
    }
    if (module.getModuleName() == oldName)
      oldTop = dyn_cast<FModuleOp>(&operation);
  }
  if (!oldTop) {
    error = "top must be an internal FIRRTL module";
    return failure();
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "WrapTop needs retained annotations";
    return failure();
  }
  std::set<std::string> bridgeNames;
  std::string bridgePrefix = "~" + oldName + "|" + oldName + ">";
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(goldengate::AnnotationClasses::BridgeIO))
      continue;
    auto target = annotation.getMember<StringAttr>("target");
    if (target && target.getValue().starts_with(bridgePrefix))
      bridgeNames.insert(
          target.getValue().drop_front(bridgePrefix.size()).str());
  }

  struct Step {
    StringAttr field;
    unsigned index = 0;
  };
  struct Leaf {
    unsigned originalPort;
    SmallVector<Step> path;
    PortInfo port;
  };
  std::set<std::string> names{"hostClock", "hostReset", oldName};
  SmallVector<PortInfo> ports;
  SmallVector<Leaf> leaves;
  ports.emplace_back(StringAttr::get(context, "hostClock"),
                     ClockType::get(context), Direction::In);
  ports.emplace_back(StringAttr::get(context, "hostReset"),
                     UIntType::get(context, 1), Direction::In);
  std::function<LogicalResult(unsigned, FIRRTLBaseType, std::string, Direction,
                              SmallVector<Step>)>
      collect = [&](unsigned original, FIRRTLBaseType type, std::string name,
                    Direction direction, SmallVector<Step> path) {
        if (auto bundle = dyn_cast<BundleType>(type)) {
          for (const auto &field : bundle.getElements()) {
            auto next = path;
            next.push_back({field.name, 0});
            Direction fieldDirection = field.isFlip
                                           ? (direction == Direction::In
                                                  ? Direction::Out
                                                  : Direction::In)
                                           : direction;
            if (failed(collect(original, field.type,
                               name + "_" + field.name.getValue().str(),
                               fieldDirection, std::move(next))))
              return failure();
          }
          return success();
        }
        if (auto vector = dyn_cast<FVectorType>(type)) {
          for (unsigned i = 0; i < vector.getNumElements(); ++i) {
            auto next = path;
            next.push_back({StringAttr(), i});
            if (failed(collect(original, vector.getElementType(),
                               name + "_" + std::to_string(i), direction,
                               std::move(next))))
              return failure();
          }
          return success();
        }
        if (!names.insert(name).second) {
          error = "wrapper port name collides: " + name;
          return failure();
        }
        PortInfo leaf(StringAttr::get(context, name), type, direction);
        ports.push_back(leaf);
        leaves.push_back({original, std::move(path), leaf});
        return success();
      };
  auto oldPorts = oldTop.getPorts();
  SmallVector<unsigned> portOrder;
  // Scala BridgeExtraction prepends each promoted bridge port. Reproduce that
  // order at the wrapper boundary while leaving original target ports in
  // their existing order.
  for (unsigned i = oldPorts.size(); i > 0; --i)
    if (bridgeNames.count(oldPorts[i - 1].getName().str()))
      portOrder.push_back(i - 1);
  for (unsigned i = 0; i < oldPorts.size(); ++i)
    if (!bridgeNames.count(oldPorts[i].getName().str()))
      portOrder.push_back(i);
  for (unsigned i : portOrder) {
    const PortInfo &port = oldPorts[i];
    if (failed(collect(i, cast<FIRRTLBaseType>(port.type),
                       port.getName().str(), oldTop.getPortDirection(i), {})))
      return failure();
  }

  OpBuilder builder(circuit.getBodyBlock(),
                    circuit.getBodyBlock()->begin());
  auto wrapper = builder.create<FModuleOp>(
      circuit.getLoc(), StringAttr::get(context, wrapperName),
      oldTop.getConventionAttr(), ports);
  builder.setInsertionPointToStart(wrapper.getBodyBlock());
  auto child = builder.create<InstanceOp>(wrapper.getLoc(), oldTop, oldName);
  for (unsigned i = 0; i < leaves.size(); ++i) {
    const Leaf &leaf = leaves[i];
    Value wrapperPort = wrapper.getBodyBlock()->getArgument(i + 2);
    Value childPort = child.getResult(leaf.originalPort);
    for (const Step &step : leaf.path) {
      if (step.field)
        childPort = builder.create<SubfieldOp>(wrapper.getLoc(), childPort,
                                               step.field.getValue());
      else
        childPort = builder.create<SubindexOp>(wrapper.getLoc(), childPort,
                                               step.index);
    }
    if (leaf.port.direction == Direction::In)
      builder.create<ConnectOp>(wrapper.getLoc(), childPort, wrapperPort);
    else
      builder.create<ConnectOp>(wrapper.getLoc(), wrapperPort, childPort);
  }

  SmallVector<Attribute> rewritten;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (annotation.isClass(goldengate::AnnotationClasses::ChannelConnection))
      rewritten.push_back(retarget(attr, oldName, true, context));
    else if (annotation.isClass(goldengate::AnnotationClasses::MemModel) ||
             annotation.isClass(goldengate::AnnotationClasses::DontTouch))
      rewritten.push_back(retarget(attr, oldName, false, context));
    else
      rewritten.push_back(attr);
  }
  auto hostAnnotation = [&](llvm::StringRef className,
                            llvm::StringRef port) {
    return DictionaryAttr::get(
        context, {builder.getNamedAttr("class",
                                       StringAttr::get(context, className)),
                  builder.getNamedAttr("target", StringAttr::get(
                      context, ("~FAMETop|FAMETop>" + port).str()))});
  };
  rewritten.push_back(hostAnnotation(goldengate::AnnotationClasses::HostClock,
                                     "hostClock"));
  rewritten.push_back(hostAnnotation(goldengate::AnnotationClasses::HostReset,
                                     "hostReset"));
  circuit->setAttr("rawAnnotations", ArrayAttr::get(context, rewritten));
  circuit.setName(wrapperName);
  return success();
}
