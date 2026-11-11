// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <optional>
#include <string>

namespace goldengate {
struct HierarchicalReference {
  circt::firrtl::FModuleLike terminal;
  std::string hierarchy, localTarget;
  llvm::StringRef reference;
};

// Resolve every explicit instance/of-module edge before treating a nonlocal
// reference as a local port or declaration selector. Keep its full context;
// callers transfer the terminal native identity without stripping hierarchy.
inline std::optional<HierarchicalReference> resolveHierarchicalReference(
    circt::firrtl::CircuitOp circuit, llvm::StringRef spelling,
    std::string &error) {
  using namespace circt::firrtl;
  auto text = spelling;
  if (!text.consume_front("~")) {
    error = "hierarchical selector must be a modern reference target";
    return std::nullopt;
  }
  auto [circuitName, local] = text.split('|');
  auto [hierarchy, reference] = local.split('>');
  if (circuitName != circuit.getName() || hierarchy.empty() || reference.empty() ||
      local.contains('|') || reference.contains('>')) {
    error = "invalid circuit or reference in hierarchical selector";
    return std::nullopt;
  }
  auto find = [&](llvm::StringRef name) -> FModuleLike {
    for (auto module : circuit.getOps<FModuleLike>())
      if (module.getModuleName() == name) return module;
    return {};
  };
  llvm::SmallVector<llvm::StringRef> path;
  hierarchy.split(path, '/');
  auto owner = find(path.front());
  if (!owner) {
    error = "hierarchical selector root module is missing";
    return std::nullopt;
  }
  for (auto edge : llvm::drop_begin(path)) {
    auto [name, child] = edge.split(':');
    auto parent = mlir::dyn_cast<FModuleOp>(owner.getOperation());
    InstanceOp selected;
    if (parent && !name.empty() && !child.empty() && !child.contains(':'))
      parent.walk([&](InstanceOp instance) {
        if (instance.getName() == name && instance.getModuleName() == child)
          selected = instance;
      });
    if (!selected || !(owner = find(child))) {
      error = "hierarchical selector instance/of-module edge does not resolve: " + edge.str();
      return std::nullopt;
    }
  }
  return HierarchicalReference{owner, "~" + circuitName.str() + "|" + hierarchy.str(),
      "~" + circuitName.str() + "|" + owner.getModuleName().str() + ">" + reference.str(), reference};
}
} // namespace goldengate
