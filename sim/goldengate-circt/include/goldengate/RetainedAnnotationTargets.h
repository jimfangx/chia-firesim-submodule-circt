// See LICENSE for license details.
#pragma once

#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "llvm/ADT/STLFunctionalExtras.h"

namespace goldengate {
// Visit actual serialized target fields, not target-looking payload strings.
// Standard target/targets members also cover user SingleTarget/Targets classes.
// Other selectors follow the known annotation schema. In particular, arbitrary
// constructor keys, printf formats, descriptions and nested dictionaries are
// opaque. Preserve record/array order, duplicates, absent/empty optional fields
// and all non-target payloads. This is not a generic substring renamer.
inline mlir::Attribute mapRetainedAnnotationTargets(
    mlir::Attribute attr,
    llvm::function_ref<mlir::StringAttr(mlir::StringAttr)> map) {
  auto dictionary = mlir::dyn_cast<mlir::DictionaryAttr>(attr);
  if (!dictionary) return attr;
  circt::firrtl::Annotation annotation(dictionary);
  auto member = [&](llvm::StringRef name) {
    if (auto target = annotation.getMember<mlir::StringAttr>(name))
      annotation.setMember(name, map(target));
    else if (auto targets = annotation.getMember<mlir::ArrayAttr>(name)) {
      llvm::SmallVector<mlir::Attribute> updated;
      for (auto target : targets)
        updated.push_back(mlir::isa<mlir::StringAttr>(target)
                              ? map(mlir::cast<mlir::StringAttr>(target)) : target);
      annotation.setMember(name, mlir::ArrayAttr::get(attr.getContext(), updated));
    }
  };
  member("target");
  member("targets");
  using A = AnnotationClasses;
  if (annotation.isClass(A::ChannelConnection)) {
    for (llvm::StringRef name : {"clock", "sources", "sinks"}) member(name);
    if (auto info = annotation.getMember<mlir::DictionaryAttr>("channelInfo")) {
      circt::firrtl::Annotation channel(info);
      if (channel.isClass(A::DecoupledForwardChannel)) {
        for (llvm::StringRef name : {"readySink", "validSource", "readySource", "validSink"})
          if (auto target = channel.getMember<mlir::StringAttr>(name))
            channel.setMember(name, map(target));
        annotation.setMember("channelInfo", channel.getAttr());
      }
    }
  } else if (annotation.isClass(A::ChannelPorts)) {
    member("clockPort"); member("ports");
  } else if (annotation.isClass(A::ModelReadPort, A::ModelWritePort,
                               A::ModelReadWritePort)) {
    for (llvm::StringRef name : {"addr", "en", "data", "mask", "wmode", "rdata", "wdata", "wmask"})
      member(name);
  } else if (annotation.isClass(A::AutoCounter, A::InternalAutoCounter,
                               A::TriggerSource, A::InternalTriggerSource,
                               A::TriggerSink, A::InternalTriggerSink,
                               A::BridgeTopWiring)) {
    member("clock"); member("reset");
  } else if (annotation.isClass(A::BridgeTopWiringOutput)) {
    for (llvm::StringRef name : {"pathlessSource", "absoluteSource", "topSink", "srcClockPort", "sinkClockPort"})
      member(name);
  } else if (annotation.isClass(A::CombinationalPath)) {
    member("sink"); member("sources");
  } else if (annotation.isClass(A::InternalXDC) ||
             annotation.isClass("midas.targetutils.xdc.XDCAnnotation")) {
    member("argumentList");
  }
  return annotation.getAttr();
}
} // namespace goldengate
