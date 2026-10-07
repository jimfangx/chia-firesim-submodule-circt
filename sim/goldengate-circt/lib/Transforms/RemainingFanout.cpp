// See LICENSE for license details.
#include "goldengate/RemainingFanout.h"
#include "goldengate/AnnotationClasses.h"
#include "goldengate/TargetUtils.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "mlir/IR/Builders.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
using namespace circt::firrtl;

LogicalResult goldengate::addRemainingFanoutAnnotations(
    CircuitOp circuit, std::string &error) {
  auto raw = circuit->getAttrOfType<ArrayAttr>("rawAnnotations");
  if (!raw) {
    error = "remaining fanout discovery requires retained post-FAME annotations";
    return failure();
  }
  struct Group {
    SmallVector<GGTarget> sources;
    SmallVector<Attribute> names;
  };
  SmallVector<Group> groups;
  for (Attribute attr : raw) {
    Annotation annotation(attr);
    if (!annotation.isClass(AnnotationClasses::ChannelConnection))
      continue;
    auto info = annotation.getMember<DictionaryAttr>("channelInfo");
    if (!info || info.getAs<StringAttr>("class") != AnnotationClasses::PipeChannel)
      continue;
    // None (bridge source) is distinct from Some(sources), as in Scala.
    if (!cast<DictionaryAttr>(attr).get("sources"))
      continue;
    auto sources = annotation.getMember<ArrayAttr>("sources");
    auto name = annotation.getMember<StringAttr>("globalName");
    if (!sources || sources.empty() || !name || name.getValue().empty()) {
      error = "model-sourced PipeChannel requires a name and nonempty source sequence";
      return failure();
    }
    SmallVector<GGTarget> identities;
    for (Attribute source : sources) {
      auto target = dyn_cast<StringAttr>(source);
      auto resolved = target
                          ? resolveAnnotationTarget(circuit, target.getValue(), error)
                          : std::nullopt;
      if (!resolved || !resolved->port ||
          resolved->module.getPortDirection(*resolved->port) != Direction::Out) {
        error = "PipeChannel '" + name.getValue().str() +
                "' source must resolve to a live output port/field";
        return failure();
      }
      identities.push_back(*resolved);
    }
    // CIRCT module/port/field identities implement ordered ReferenceTarget
    // equality. Latency, sinks and clocks intentionally do not enter this key.
    auto sameSources = [&](const Group &group) {
      if (group.sources.size() != identities.size())
        return false;
      for (unsigned i = 0; i < identities.size(); ++i)
        if (group.sources[i].module != identities[i].module ||
            group.sources[i].port != identities[i].port ||
            group.sources[i].fieldID != identities[i].fieldID)
          return false;
      return true;
    };
    auto found = llvm::find_if(groups, sameSources);
    if (found == groups.end()) {
      groups.push_back({std::move(identities), {name}});
    } else if (!llvm::is_contained(found->names, name)) {
      found->names.push_back(name);
    }
  }
  OpBuilder builder(circuit.getContext());
  SmallVector<Attribute> updated(raw.begin(), raw.end());
  for (const auto &group : groups)
    if (group.names.size() > 1)
      updated.push_back(builder.getDictionaryAttr({
          builder.getNamedAttr("class", builder.getStringAttr(AnnotationClasses::ChannelFanout)),
          builder.getNamedAttr("channelNames", builder.getArrayAttr(group.names))}));
  circuit->setAttr("rawAnnotations", builder.getArrayAttr(updated));
  return success();
}
