// See LICENSE for license details.
#pragma once

#include "goldengate/AnnotationClasses.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <optional>
#include <string>

namespace goldengate {
// SFC hostDecouplingRenames maps wrapper and model data references to their
// payload leaves. Preserve symbol names and visibility so InnerRefs survive.
// Collect before editing any ports so unsupported metadata fails atomically.
inline mlir::FailureOr<unsigned> collectFAMEPayloadSymbols(
    circt::firrtl::FModuleOp module, unsigned port,
    circt::firrtl::BundleType channel, llvm::StringRef leaf,
    llvm::SmallVectorImpl<circt::hw::InnerSymPropertiesAttr> &symbols,
    std::string &error) {
  using namespace circt::firrtl;
  auto bitsIndex = channel.getElementIndex("bits");
  if (!bitsIndex) {
    error = "FAME channel has no payload field for data identities";
    return mlir::failure();
  }
  auto type = channel.getElements()[*bitsIndex].type;
  unsigned fieldID = channel.getFieldID(*bitsIndex);
  if (!leaf.empty()) {
    auto bundle = mlir::dyn_cast<BundleType>(type);
    auto index = bundle ? bundle.getElementIndex(leaf) : std::nullopt;
    if (!index) {
      error = "FAME data identity payload leaf is absent";
      return mlir::failure();
    }
    fieldID += bundle.getFieldID(*index);
    type = bundle.getElements()[*index].type;
  }
  if (type != module.getPorts()[port].type ||
      mlir::isa<BundleType, FVectorType>(type)) {
    error = "FAME data identity requires a matching ground payload";
    return mlir::failure();
  }
  auto sym = module.getPorts()[port].sym;
  if (sym)
    for (auto property : sym) {
      if (property.getFieldID() != 0) {
        error = "FAME ground-port symbol has a nonzero field ID";
        return mlir::failure();
      }
      for (auto existing : symbols)
        if (existing.getName() == property.getName() ||
            existing.getFieldID() == fieldID) {
          error = "FAME payload symbols have duplicate identities";
          return mlir::failure();
        }
      symbols.push_back(circt::hw::InnerSymPropertiesAttr::get(
          module.getContext(), property.getName(), fieldID,
          property.getSymVisibility()));
    }
  return fieldID;
}

// Model DontTouch annotations are consumed separately, as in SFC. Only retained
// wrapper protections transfer here; model identities do not create protection.
inline mlir::LogicalResult collectFAMEWrapperPayloadMetadata(
    circt::firrtl::FModuleOp top, unsigned port,
    circt::firrtl::BundleType channel, llvm::StringRef leaf,
    llvm::SmallVectorImpl<circt::firrtl::Annotation> &annotations,
    llvm::SmallVectorImpl<circt::hw::InnerSymPropertiesAttr> &symbols,
    std::string &error) {
  using namespace circt::firrtl;
  auto fieldID = collectFAMEPayloadSymbols(top, port, channel, leaf, symbols, error);
  if (mlir::failed(fieldID))
    return mlir::failure();
  for (auto anno : AnnotationSet::forPort(top, port)) {
    auto oldID = anno.getDict().get("circt.fieldID");
    auto integerID = mlir::dyn_cast_or_null<mlir::IntegerAttr>(oldID);
    if (!anno.isClass(AnnotationClasses::DontTouch) ||
        anno.getDict().get("target") ||
        (oldID && (!integerID || !integerID.getValue().isZero()))) {
      error = "FAME wrapper data port annotation requires a payload transfer policy";
      return mlir::failure();
    }
    annotations.emplace_back(anno, *fieldID);
  }
  return mlir::success();
}
} // namespace goldengate
