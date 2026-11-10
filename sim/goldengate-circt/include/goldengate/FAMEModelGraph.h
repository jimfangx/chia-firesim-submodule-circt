// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "goldengate/FAME5Models.h"
#include <string>

namespace goldengate {
struct FAMEModelGraphResult {
  unsigned definitions = 0;
  unsigned instances = 0;
  unsigned clockDomains = 0;
  unsigned dataEndpoints = 0;
  unsigned threadedGroups = 0;
  unsigned sharedThreads = 0;
};
// Compose a prepared InferModelPorts graph into real FAME hardware and optional
// simulator transport. Select definitions from FAMETransformAnnotation; preserve
// annotation order, local dependency/clock identities and independent instances.
// Requires an uninstantiated host-clock/reset top, directly promoted models,
// one uniquely instantiated clock hub (one or more flat Clock domains) for
// simulator transport, and
// scalar integer data ports or grouped integer channels, including shared definitions.
// Non-hubs have one virtual scalar clock; repeated definitions share their
// channel ABI, never state. Models may contain state or memories supported by
// dependency analysis. Unknown target-bearing metadata, incomplete coverage,
// uncovered scalar ports and incompatible shared channel ABIs reject. Exclusive
// scalar PipeChannel top-input/output identities become complete Decoupled
// loopbacks, with payload targets and wrapper metadata transferred. Sharing
// a scalar input with a model requires ChannelExcision to split and annotate
// the fanout first; an unsplit scalar rejects atomically.
// With transport, join retained pipe/fanout and ready/valid endpoints, convert
// target clock tokens to Boolean lanes and activate the simulator wrapper.
// Boundary mode also admits graphs composed only of scalar virtual-clock
// models, as SFC FAMETransform does; SFC SimWrapper still requires one hub.
// When enableMultiThreading is true, group selected repeated models after all
// FAME controls/channel ABIs are complete and before simulator transport. Choose
// threadingClocks explicitly for the platform. Record physical allocation and
// keep top-level logical channel/clock identities used by transport unchanged.
// No clock producer, widget/platform or timing RAM replacement is implied. False leaves the post-FAME boundary for oracle comparison.
// Clone/verify/commit makes the complete operation atomic. On success all op,
// port, hierarchy, target and state analyses must be rebuilt; result is updated
// only on success. Retained annotations and gate/XDC identities are transferred
// by their existing schema-aware policies; unsupported schemas fail explicitly.
mlir::LogicalResult rewriteFAMEModelGraph(
    circt::firrtl::CircuitOp circuit, FAMEModelGraphResult &result,
    std::string &error, bool withTransport = true,
    bool enableMultiThreading = false,
    FAME5ClockSemantics threadingClocks = FAME5ClockSemantics::Abstract);
} // namespace goldengate
