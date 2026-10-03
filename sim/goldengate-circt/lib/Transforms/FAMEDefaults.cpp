// See LICENSE for license details.
#include "goldengate/FAMEDefaults.h"
#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/BuiltinAttributes.h"
#include <set>

using namespace circt::firrtl;
using namespace mlir;

LogicalResult goldengate::addFAMEDefaults(CircuitOp circuit,
                                           std::string &error) {
  FModuleOp top;
  for (Operation &op : circuit.getBodyBlock()->getOperations())
    if (auto module = dyn_cast<FModuleOp>(&op);
        module && module.getName() == circuit.getName())
      top = module;
  if (!top) {
    error = "FAMEDefaults requires an internal circuit top";
    return failure();
  }
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "FAMEDefaults requires retained annotations";
    return failure();
  }

  // Scala's channel namespace is seeded by existing global channel names.
  std::set<std::string> channelNames;
  SmallVector<Attribute> annotations(raw.begin(), raw.end());
  SmallVector<Attribute> loopbacks, modelLabels;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto name = annotation.getMember<StringAttr>("globalName");
    if (!name) {
      error = "channel annotation is missing globalName";
      return failure();
    }
    channelNames.insert(name.getValue().str());
  }
  auto text = [&](StringRef value) -> Attribute {
    return StringAttr::get(circuit.getContext(), value);
  };
  std::string prefix = "~" + circuit.getName().str() + "|";
  std::string topPrefix = prefix + top.getName().str() + ">";

  // FAMEDefaults walks the top body in source order.  CIRCT gives each
  // instance port its own SSA result, so no textual FIRRTL expression parsing
  // is needed to identify a direct inter-model connection.
  top.walk([&](Operation *op) {
    if (auto instance = dyn_cast<InstanceOp>(op)) {
      modelLabels.push_back(DictionaryAttr::get(
          circuit.getContext(),
          {{StringAttr::get(circuit.getContext(), "class"),
            text(AnnotationClasses::FAMETransform)},
           {StringAttr::get(circuit.getContext(), "target"),
            text(prefix + instance.getModuleName().str())}}));
      return;
    }
    auto addLoopback = [&](Value dest, Value source) {
      auto lhs = dyn_cast<OpResult>(dest);
      auto rhs = dyn_cast<OpResult>(source);
      if (!lhs || !rhs || isa<ClockType>(dest.getType()) ||
          isa<ClockType>(source.getType()))
        return;
      auto lhsInstance = dyn_cast<InstanceOp>(lhs.getOwner());
      auto rhsInstance = dyn_cast<InstanceOp>(rhs.getOwner());
      if (!lhsInstance || !rhsInstance)
        return;
      auto lhsPort = lhsInstance.getPortNameStr(lhs.getResultNumber());
      auto rhsPort = rhsInstance.getPortNameStr(rhs.getResultNumber());
      std::string base = rhsInstance.getName().str() + "_" + rhsPort.str() +
                         "__to__" + lhsInstance.getName().str() + "_" +
                         lhsPort.str();
      std::string name = base;
      for (unsigned suffix = 0; channelNames.count(name); ++suffix)
        name = base + "_" + std::to_string(suffix);
      channelNames.insert(name);
      auto context = circuit.getContext();
      auto pipe = DictionaryAttr::get(
          context,
          {{StringAttr::get(context, "class"), text(AnnotationClasses::PipeChannel)},
           {StringAttr::get(context, "latency"),
            IntegerAttr::get(IntegerType::get(context, 64), 0)}});
      loopbacks.push_back(DictionaryAttr::get(
          context,
          {{StringAttr::get(context, "class"),
            text(AnnotationClasses::ChannelConnection)},
           {StringAttr::get(context, "globalName"), text(name)},
           {StringAttr::get(context, "channelInfo"), pipe},
           {StringAttr::get(context, "sources"),
            ArrayAttr::get(context, {text(topPrefix +
                                          rhsInstance.getName().str() +
                                          "." + rhsPort.str())})},
           {StringAttr::get(context, "sinks"),
            ArrayAttr::get(context, {text(topPrefix +
                                          lhsInstance.getName().str() +
                                          "." + lhsPort.str())})}}));
    };
    if (auto connect = dyn_cast<ConnectOp>(op))
      addLoopback(connect.getDest(), connect.getSrc());
    else if (auto connect = dyn_cast<StrictConnectOp>(op))
      addLoopback(connect.getDest(), connect.getSrc());
  });
  // The SFC pass appends loopbacks before model labels, regardless of the
  // position of instance declarations in the top body.
  annotations.append(loopbacks.begin(), loopbacks.end());
  annotations.append(modelLabels.begin(), modelLabels.end());
  circuit->setAttr("rawAnnotations",
                   ArrayAttr::get(circuit.getContext(), annotations));
  return success();
}
