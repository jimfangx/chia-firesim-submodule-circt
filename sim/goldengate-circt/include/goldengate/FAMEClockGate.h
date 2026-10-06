// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <map>
#include <string>

namespace goldengate {
// Preserve the original clock identity on the actual gate operation, including
// through namespace allocation, instance renaming and serialized boundaries.
inline constexpr const char *fameClockGateAttr = "goldengate.fameClockGate";

class FAMEClockGateIndex {
public:
  mlir::LogicalResult collect(circt::firrtl::FModuleOp model,
                              std::string &error) {
    clocks.clear();
    names.clear();
    auto result = mlir::success();
    model.walk([&](mlir::Operation *op) {
      auto gate = mlir::dyn_cast<circt::firrtl::InstanceOp>(op);
      if (gate && !names.emplace(gate.getName().str(), gate).second) {
        error = "ambiguous FAME clock-gate instance name";
        result = mlir::failure();
      }
      auto attr = op->getAttr(fameClockGateAttr);
      if (!attr)
        return;
      auto clock = mlir::dyn_cast<mlir::StringAttr>(attr);
      if (!gate || !clock || clock.getValue().empty() ||
          gate.getModuleName() != "AbstractClockGate" ||
          gate.getNumResults() != 3 || gate.getPortNameStr(0) != "I" ||
          gate.getPortNameStr(1) != "CE" || gate.getPortNameStr(2) != "O" ||
          gate.getPortDirection(0) != circt::firrtl::Direction::In ||
          gate.getPortDirection(1) != circt::firrtl::Direction::In ||
          gate.getPortDirection(2) != circt::firrtl::Direction::Out ||
          !mlir::isa<circt::firrtl::ClockType>(gate.getResult(0).getType()) ||
          !mlir::isa<circt::firrtl::ClockType>(gate.getResult(2).getType()) ||
          gate.getResult(1).getType() !=
              circt::firrtl::UIntType::get(model.getContext(), 1)) {
        error = "invalid FAME clock-gate identity or port schema";
        result = mlir::failure();
      } else if (!clocks.emplace(clock.getValue().str(), gate).second) {
        error = "duplicate FAME clock-gate identity for " + clock.getValue().str();
        result = mlir::failure();
      }
    });
    return result;
  }

  circt::firrtl::InstanceOp lookup(llvm::StringRef clock,
                                  bool allowLegacy = true) const {
    if (auto it = clocks.find(clock.str()); it != clocks.end())
      return it->second;
    // Compatibility for read-only consumers of older, entirely untagged
    // boundaries. A native model must never bind a colliding target instance.
    if (allowLegacy && clocks.empty())
      if (auto it = names.find((clock + "_buffer").str()); it != names.end())
        return it->second;
    return {};
  }

private:
  std::map<std::string, circt::firrtl::InstanceOp> clocks, names;
};
} // namespace goldengate
