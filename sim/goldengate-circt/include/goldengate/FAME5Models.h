// See LICENSE for license details.
#pragma once
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
/// Abstract preserves the SFC simulation gate and rejects dynamic virtual clocks.
/// XilinxBUFGCE additionally runs the real Xilinx host specialization after
/// grouping, retaining the primitive's default synchronous low-phase CE latch.
/// This is an explicit platform choice, never an inferred clock rewrite.
enum class FAME5ClockSemantics { Abstract, XilinxBUFGCE };
/// Share repeated, annotated top-level FAME models using SFC's static host-cycle
/// round-robin scheduler, channel mux/demux and per-thread state. Call on the
/// post-FAME circuit with canonical whole-bundle channel connects (or their
/// import-expanded ready/valid/bits equivalent) and lowered
/// scalar register assignments. The implementation supports independent groups
/// of models and its acyclic internal hierarchy, with canonical clock gates,
/// passive known-width payloads and scalar synchronous registers
/// and zero/one-cycle-read, one-cycle-write scalar UInt/SInt target memories. Synchronous
/// target clocks may be hostClock or always-enabled canonical AbstractClockGate
/// outputs driven by hostClock;
/// response banks use the same internal free-running selector as register state.
/// Every reachable internal definition is cloned/banked once, preserving each
/// child instance. Missing hostClock/hostReset inputs are appended and wired to
/// the parent host ports, as in SFC AddHostClockAndReset. Existing child host
/// ports must already use the canonical parent host connection. Each module
/// retains its own free-running selector. Canonical read-only plusarg_reader
/// configuration sources retain their external definition and parameters. Other
/// unknown externals, recursive cycles,
/// bind/layer instances and retained debug targets needing renames reject
/// atomically. The original definitions remain, as in MultiThreadFAME5Models.
/// Explicit XilinxBUFGCE mode also supports the canonical FAMETransform dynamic
/// virtual controller for scalar registers and zero/one-cycle-read target memories.
/// It preserves target clock edges using real BUFGCE instances and banks
/// synchronous responses under the unchanged SFC host-clock edge/tag schedule.
/// Abstract mode keeps the SFC local unreset selector startup-phase relation.
/// Production Xilinx mode resets every local selector with hostReset, establishing
/// alignment with the arbiter and resets every canonical host-control bank.
/// Target register/token resets and target RAM contents remain unchanged. The caller must provide
/// protocol-valid irrevocable channel inputs and ordinary target reset tokens.
/// Consumed FAMETransform/FAMEChannelPorts selections retain their unchanged
/// source-definition provenance, as SFC does; live transport endpoints remain
/// top-level ChannelConnection targets. The circuit's goldengate.fame5.groups
/// allocation records thread order, live channel peers and cloned definitions.
/// No repeated group is a successful no-op. All mutation is staged and verified.
mlir::LogicalResult multiThreadFAME5Models(circt::firrtl::CircuitOp circuit,
                                          unsigned &threadedGroups,
                                          std::string &error,
                                          FAME5ClockSemantics semantics = FAME5ClockSemantics::Abstract);
} // namespace goldengate
