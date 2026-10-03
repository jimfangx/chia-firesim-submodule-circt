// See LICENSE for license details.
#pragma once

#include "goldengate/ChannelAnalysis.h"
#include "goldengate/HierarchyAnalysis.h"
#include <optional>
#include <string>

namespace goldengate {
// InferModelPorts' local channel annotation, resolved to CIRCT module ports.
struct ModelPortGroup {
  std::string name;
  circt::firrtl::FModuleLike module;
  circt::firrtl::Direction direction;
  std::optional<unsigned> clockPort;
  llvm::SmallVector<unsigned> ports;
};

struct ModelChannelBinding {
  std::string globalName;
  const ModelPortGroup *portGroup;
  circt::firrtl::InstanceOp instance;
  llvm::SmallVector<unsigned> instancePorts;
};

std::optional<ModelPortGroup>
analyzeModelPortGroup(circt::firrtl::CircuitOp circuit,
                      circt::firrtl::Annotation annotation,
                      std::string &error);

// Join a global channel to local model ports through FIRRTL SSA connections.
// This deliberately compares port identities: reverse decoupled and target
// clock channels need not share their local FAMEChannelPorts name.
std::optional<llvm::SmallVector<ModelChannelBinding>>
bindChannelToModels(const GGChannelConnection &channel,
                    const TopHierarchy &hierarchy,
                    llvm::ArrayRef<ModelPortGroup> portGroups,
                    std::string &error);
} // namespace goldengate
