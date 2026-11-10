// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <optional>

namespace goldengate {
// Read the existing boundary type before building forwarding adapters. Keep
// addresses intact across U250 (25 bits) and F2 (32 bits); subsequent exact
// schema checks also validate all peer ports and field directions.
inline std::optional<unsigned> controlPortWidth(
    circt::firrtl::FModuleOp module, llvm::StringRef path) {
  auto [name, fields] = path.split('.');
  mlir::Type type;
  for (auto port : module.getPorts())
    if (port.name.getValue() == name) type = port.type;
  while (type && !fields.empty()) {
    auto [field, rest] = fields.split('.');
    auto bundle = mlir::dyn_cast<circt::firrtl::BundleType>(type);
    auto element = bundle ? bundle.getElement(field) : std::nullopt;
    if (!element) return std::nullopt;
    type = element->type;
    fields = rest;
  }
  auto uint = mlir::dyn_cast_or_null<circt::firrtl::UIntType>(type);
  if (!uint || !uint.getWidth() ||
      (*uint.getWidth() != 25 && *uint.getWidth() != 32)) return std::nullopt;
  return unsigned(*uint.getWidth());
}
} // namespace goldengate
