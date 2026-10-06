// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <map>
#include <string>

namespace goldengate {
// Internal identity attached to generated host state. Unlike a declaration
// name, this survives uniquing and does not alias target state with that name.
// Requires a module body; no IR mutation or annotations consumed/produced.
// Recollect after register creation or channel-identity changes.
inline constexpr const char *fameFiredChannelAttr = "goldengate.fameFiredChannel";

class FAMEFiredRegisterIndex {
public:
  mlir::LogicalResult collect(circt::firrtl::FModuleOp module,
                              std::string &error) {
    channels.clear();
    names.clear();
    mlir::LogicalResult result = mlir::success();
    module.walk([&](mlir::Operation *op) {
      if (auto reg = mlir::dyn_cast<circt::firrtl::RegOp>(op))
        names.emplace(reg.getName().str(), reg.getResult());
      if (auto reg = mlir::dyn_cast<circt::firrtl::RegResetOp>(op))
        names.emplace(reg.getName().str(), reg.getResult());
      auto attr = op->getAttr(fameFiredChannelAttr);
      if (!attr)
        return;
      auto channel = mlir::dyn_cast<mlir::StringAttr>(attr);
      auto reg = mlir::dyn_cast<circt::firrtl::RegResetOp>(op);
      if (!channel || channel.getValue().empty() || !reg) {
        error = "invalid FAME fired channel identity";
        result = mlir::failure();
      } else if (!channels.emplace(channel.getValue().str(), reg.getResult())
                      .second) {
        error = "duplicate FAME fired channel identity for " +
                channel.getValue().str();
        result = mlir::failure();
      }
    });
    return result;
  }

  mlir::Value lookup(const std::string &channel, bool allowLegacy = true) const {
    if (auto it = channels.find(channel); it != channels.end())
      return it->second;
    // Read-only compatibility for old, already transformed SFC boundaries.
    // A model with native identities must never fall back to target names.
    if (!allowLegacy || !channels.empty())
      return {};
    for (const auto &suffix : {"_fired_0", "_fired"})
      if (auto it = names.find(channel + suffix); it != names.end())
        return it->second;
    return {};
  }

private:
  std::map<std::string, mlir::Value> channels, names;
};
} // namespace goldengate
