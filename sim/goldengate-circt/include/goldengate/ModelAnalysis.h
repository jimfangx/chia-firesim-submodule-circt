// See LICENSE for license details.
#pragma once

#include "goldengate/ChannelAnalysis.h"
#include "goldengate/HierarchyAnalysis.h"
#include <memory>
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

// A caller-owned snapshot for the immutable FAME planning phase. Bindings retain
// pointers into this object; destroy all plans and this snapshot before changing
// annotations, module signatures or connectivity. It is intentionally immovable.
class PreparedFAMEAnalysis {
public:
  static std::unique_ptr<PreparedFAMEAnalysis>
  create(circt::firrtl::CircuitOp circuit, std::string &error);
  ~PreparedFAMEAnalysis();
  PreparedFAMEAnalysis(const PreparedFAMEAnalysis &) = delete;
  PreparedFAMEAnalysis &operator=(const PreparedFAMEAnalysis &) = delete;

  circt::firrtl::CircuitOp circuit() const;
  const TopHierarchy &hierarchy() const;
  llvm::ArrayRef<ModelPortGroup> groups() const;
  llvm::ArrayRef<GGChannelConnection> channels() const;
  llvm::ArrayRef<const TopPortConnection *>
  connectionsForTopPort(unsigned port) const;
  // Lazy to preserve data/clock queries' intentional exclusion of TargetClock
  // bindings. A successful query is shared across all model selections.
  const llvm::SmallVector<ModelChannelBinding> *
  bindingsFor(unsigned channel, std::string &error);

private:
  struct Impl;
  explicit PreparedFAMEAnalysis(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

std::optional<ModelPortGroup>
analyzeModelPortGroup(circt::firrtl::CircuitOp circuit,
                      circt::firrtl::Annotation annotation,
                      std::string &error);

// Join a global channel to local model ports through FIRRTL SSA connections.
// This deliberately compares port identities: reverse decoupled and target
// clock channels need not share their local FAMEChannelPorts name.
// A global clock exported by another module has no local clockPort: SFC
// assigns that model a virtual clock channel. The clock-owning model must
// still have an exact local port/instance match.
std::optional<llvm::SmallVector<ModelChannelBinding>>
bindChannelToModels(const GGChannelConnection &channel,
                    const TopHierarchy &hierarchy,
                    llvm::ArrayRef<ModelPortGroup> portGroups,
                    std::string &error);
} // namespace goldengate
