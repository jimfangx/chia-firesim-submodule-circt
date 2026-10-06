// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <map>
#include <string>

namespace goldengate {
// Generated host state is identified by its original model clock, independent
// of declaration uniquing/renaming. Requires a module body; consumes/produces
// no annotations and does not mutate IR. Recollect after state creation.
inline constexpr const char *fameClockEnableAttr = "goldengate.fameClockEnable";

class FAMEClockEnableIndex {
public:
  mlir::LogicalResult collect(circt::firrtl::FModuleOp model,
                              std::string &error) {
    clocks.clear();
    names.clear();
    auto result = mlir::success();
    model.walk([&](mlir::Operation *op) {
      auto reg = mlir::dyn_cast<circt::firrtl::RegResetOp>(op);
      if (reg)
        names.emplace(reg.getName().str(), reg.getResult());
      auto attr = op->getAttr(fameClockEnableAttr);
      if (!attr)
        return;
      auto clock = mlir::dyn_cast<mlir::StringAttr>(attr);
      if (!reg || !clock || clock.getValue().empty()) {
        error = "invalid FAME clock-enable identity";
        result = mlir::failure();
      } else if (!clocks.emplace(clock.getValue().str(), reg.getResult()).second) {
        error = "duplicate FAME clock-enable identity for " + clock.getValue().str();
        result = mlir::failure();
      }
    });
    return result;
  }

  mlir::Value lookup(llvm::StringRef clock, bool allowLegacy = true) const {
    if (auto it = clocks.find(clock.str()); it != clocks.end())
      return it->second;
    // Read-only compatibility with older transformed boundaries. Once native
    // identities exist, a target declaration must never impersonate host state.
    if (allowLegacy && clocks.empty())
      if (auto it = names.find((clock + "_enabled").str()); it != names.end())
        return it->second;
    return {};
  }

private:
  std::map<std::string, mlir::Value> clocks, names;
};

inline mlir::Value lookupFAMEClockEnable(circt::firrtl::FModuleOp model,
                                        llvm::StringRef clock,
                                        std::string &error) {
  FAMEClockEnableIndex enables;
  if (mlir::failed(enables.collect(model, error)))
    return {};
  auto value = enables.lookup(clock);
  if (!value)
    error = "missing buffered FAME clock enable for " + clock.str();
  return value;
}
} // namespace goldengate
