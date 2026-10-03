// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>
#include <string>
#include <vector>

namespace goldengate {
struct GGBridgeInstance {
  circt::firrtl::InstanceOp instance;
  circt::firrtl::FModuleLike module;
  std::vector<std::string> path;
  std::string widgetClass;
  std::vector<std::string> channelNames;
  mlir::DictionaryAttr sourceAnnotation;
};

// The BridgeExtraction.annotateInstances boundary: resolve module-level
// BridgeAnnotations to reachable CIRCT instance operations and expand the
// bridge channel names used when that instance is promoted to the top.
std::optional<llvm::SmallVector<GGBridgeInstance>>
analyzeBridgeInstances(circt::firrtl::CircuitOp circuit, std::string &error);

// Promote directly instantiated bridge ports to a single, directioned
// top-level bundle, as in ExtractBridges. Aggregate promotion preserves the
// FIRRTL bundle flips and vector shape. Analog ports remain unsupported.
mlir::LogicalResult promoteBridgePorts(circt::firrtl::CircuitOp circuit,
                                       std::string &error,
                                       bool includeAggregates = false);
} // namespace goldengate
