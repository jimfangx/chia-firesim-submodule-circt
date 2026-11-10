// See LICENSE for license details.
#pragma once
#include "goldengate/ControlPortWidth.h"
#include <string>

namespace goldengate {
// Use the actual queued/unbuffered host AXI boundaries as the platform contract.
// Retained metadata must agree with both address channels on every host. Never
// choose a width from MMIO sizes: small widgets still need all F2 address bits
// to reach the error endpoint instead of aliasing an allocated low address.
inline std::optional<unsigned> printControlAddressBits(
    circt::firrtl::CircuitOp circuit, std::string &error) {
  std::optional<unsigned> selected;
  for (auto module : circuit.getOps<circt::firrtl::FModuleOp>()) {
    auto info = module->getAttrOfType<mlir::DictionaryAttr>("goldengate.printHost");
    if (!info) continue;
    auto metadata = info.getAs<mlir::IntegerAttr>("addressBits");
    auto aw = controlPortWidth(module, "ctrl.aw.bits.addr");
    auto ar = controlPortWidth(module, "ctrl.ar.bits.addr");
    if (!metadata || metadata.getValue().getBitWidth() > 64 ||
        !aw || !ar || *aw != *ar ||
        metadata.getInt() != *aw || (selected && *selected != *aw)) {
      error = "Print control requires consistent typed 25/32-bit AW/AR host boundaries and address metadata";
      return std::nullopt;
    }
    selected = aw;
  }
  if (!selected)
    error = "Print control requires at least one live host address boundary";
  return selected;
}
} // namespace goldengate
