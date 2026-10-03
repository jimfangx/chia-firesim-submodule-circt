// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <optional>
#include <string>

namespace goldengate {
// SFC hostDecouplingRenames maps each retained wrapper data reference to its
// payload leaf. Attached CIRCT annotations encode that leaf with a field ID.
// Collect before editing any ports so unsupported metadata fails atomically.
inline mlir::LogicalResult collectFAMEWrapperPayloadAnnotations(
    circt::firrtl::FModuleOp top, unsigned port,
    circt::firrtl::BundleType channel, llvm::StringRef leaf,
    llvm::SmallVectorImpl<circt::firrtl::Annotation> &result,
    std::string &error) {
  using namespace circt::firrtl;
  auto bitsIndex = channel.getElementIndex("bits");
  if (!bitsIndex) {
    error = "FAME channel has no payload field for wrapper annotations";
    return mlir::failure();
  }
  auto type = channel.getElements()[*bitsIndex].type;
  unsigned fieldID = channel.getFieldID(*bitsIndex);
  if (!leaf.empty()) {
    auto bundle = mlir::dyn_cast<BundleType>(type);
    auto index = bundle ? bundle.getElementIndex(leaf) : std::nullopt;
    if (!index) {
      error = "FAME wrapper annotation payload leaf is absent";
      return mlir::failure();
    }
    fieldID += bundle.getFieldID(*index);
    type = bundle.getElements()[*index].type;
  }
  if (type != top.getPorts()[port].type ||
      mlir::isa<BundleType, FVectorType>(type)) {
    error = "FAME wrapper annotation requires a matching ground payload";
    return mlir::failure();
  }
  if (top.getPorts()[port].sym) {
    error = "FAME wrapper data port symbol requires a payload field transfer";
    return mlir::failure();
  }
  for (auto anno : AnnotationSet::forPort(top, port)) {
    auto oldID = anno.getDict().get("circt.fieldID");
    auto integerID = mlir::dyn_cast_or_null<mlir::IntegerAttr>(oldID);
    if (!anno.isClass("firrtl.transforms.DontTouchAnnotation") ||
        anno.getDict().get("target") ||
        (oldID && (!integerID || !integerID.getValue().isZero()))) {
      error = "FAME wrapper data port annotation requires a payload transfer policy";
      return mlir::failure();
    }
    result.emplace_back(anno, fieldID);
  }
  return mlir::success();
}
} // namespace goldengate
