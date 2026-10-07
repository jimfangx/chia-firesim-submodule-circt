// See LICENSE for license details.
#pragma once

#include "goldengate/ModelAnalysis.h"
#include "goldengate/CombDependencyAnalysis.h"
#include <optional>
#include <string>

namespace goldengate {
// The top-level channel interface that FAMETransform will create. These are
// typed model bindings until the point where a new FIRRTL port name is needed.
struct FAMETopChannelPort {
  const ModelChannelBinding *binding;
  std::string portName;
  circt::firrtl::BundleType type;
};

struct FAMEPortPlan {
  llvm::SmallVector<FAMETopChannelPort> sinks;
  llvm::SmallVector<FAMETopChannelPort> sources;
  llvm::SmallVector<unsigned> staleTopPorts;
};

// Capture the hub's domain identities before channelization deletes the scalar
// ports. Order is FAMEChannelPorts/TargetClockChannel annotation order, never
// physical module port order. An empty payloadField denotes scalar bits.
struct FAMEHubClockDomain {
  unsigned modelPort;
  unsigned topPort;
  std::string modelClockName;
  std::string topClockName;
  std::string payloadField;
  RationalClockInfo clockInfo;
};

std::optional<llvm::SmallVector<FAMEHubClockDomain>>
analyzeFAMEHubClockDomains(const GGChannelConnection &channel,
                           const TopHierarchy &hierarchy,
                           const ModelChannelBinding &binding,
                           std::string &error);

// Stable data-channel assignment captured before hub/alias ports are erased.
// Input FSMs use this domain's raw token; output FSMs use its enabled register.
struct FAMEChannelClockDomain {
  std::string globalName;
  std::string localName;
  circt::firrtl::Direction direction;
  std::string modelClockName;
};

std::optional<llvm::SmallVector<FAMEChannelClockDomain>>
analyzeFAMEChannelClockDomains(circt::firrtl::CircuitOp circuit,
                              circt::firrtl::FModuleOp model,
                              llvm::ArrayRef<FAMEHubClockDomain> domains,
                              std::string &error);

// Capture names and dependencies before any port rewrite invalidates indices.
// Selection follows connection annotation order; payload order stays in the
// connection's source list, independently of the model's physical port order.
struct FAMEOutputSelection {
  std::string globalName;
  std::string localName;
  ChannelKind kind;
  unsigned fieldCount;
  LocalChannelDependency dependency;
};

std::optional<llvm::SmallVector<FAMEOutputSelection>>
analyzeFAMEOutputSelection(circt::firrtl::CircuitOp circuit,
                           circt::firrtl::FModuleOp model, std::string &error);

std::optional<FAMEPortPlan>
analyzeFAMEPorts(const TopHierarchy &hierarchy,
                 llvm::ArrayRef<ModelChannelBinding> bindings,
                 llvm::ArrayRef<GGChannelConnection> channels,
                 llvm::ArrayRef<circt::firrtl::FModuleLike> transformedModules,
                 std::string &error);
} // namespace goldengate
