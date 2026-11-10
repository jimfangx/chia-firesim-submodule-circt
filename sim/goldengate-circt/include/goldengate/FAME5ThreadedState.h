// See LICENSE for license details.
#ifndef GOLDENGATE_FAME5_THREADED_STATE_H
#define GOLDENGATE_FAME5_THREADED_STATE_H

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include <string>

namespace goldengate {
/// Recognize the unchanged SFC AbstractClockGate external ABI and constant-one
/// CE driven by hostClock. Dynamic enables never satisfy this predicate.
bool isFAME5ClockGate(circt::firrtl::InstanceOp instance);
/// Recognize SFC's virtual-clock host controller for diagnostics/preflight:
/// CE=(enabled & finishing) & ~hostReset, zero-reset host enabled flag updated
/// by mux(finishing,1,enabled). Validate original registers or equivalent banks.
/// Abstract-clock grouping rejects this shape atomically. Explicit Xilinx
/// grouping additionally preserves the production BUFGCE low-phase CE latch;
/// that platform choice is made by the grouping API, never by this predicate.
bool isFAME5VirtualClockController(circt::firrtl::InstanceOp instance);
/// Replace scalar registers with thread-indexed asynchronous-read memories,
/// following midas.passes.fame.MuxingMultiThreader.onStmt. The caller supplies
/// the active thread index and retains responsibility for channel scheduling.
/// Call after lowering whens/register connects, before target clock lowering.
/// Every register must have one unconditional, whole-register connect. Reset
/// affects only the selected slot, on the original register clock edge. Optional
/// production broadcastHostReset resets every slot only for canonical constant-
/// initialized hostClock/hostReset control registers, using distinct reset write
/// ports while masking the normal writer. Target reset/token semantics remain
/// selected-thread, and target memories are never cleared. Asynchronous resets, aggregates,
/// symbolic/annotated registers and nested declarations reject atomically.
mlir::LogicalResult threadFAME5Registers(circt::firrtl::FModuleOp module,
                                        unsigned threadCount,
                                        mlir::Value threadIndex,
                                        std::string &error,
                                        bool broadcastHostReset = false);
/// Clock interpretation for the checked synchronous target-memory accessors.
/// Dynamic virtual controllers require explicit production Xilinx semantics.
enum class FAME5MemoryClockSemantics { Abstract, XilinxBUFGCE };
/// Bank zero/one-cycle-read, one-cycle-write target memories using the SFC
/// ImplementThreadedMems address mapping: {addr, thread} for power-of-two
/// counts, per-accessor unreset host-clock base counters for other counts.
/// Geometry, masks, readwriters, clocks/enables and read-under-write attributes
/// are retained. One-cycle reads use SFC's per-accessor host-clock response banks
/// and delayed thread/target-edge pipelines (additional latency for >=4 threads).
/// The caller must provide SFC's free-running round-robin thread schedule and
/// startup alignment; helper registers intentionally have no reset. Synchronous
/// reads require lowered scalar UInt/SInt data, hostClock and a common input target clock
/// or always-enabled canonical AbstractClockGate outputs driven by hostClock.
/// Explicit Xilinx semantics also admit the exact canonical virtual controller
/// and retain its BUFGCE low-phase latch. Other generated clocks reject; gate enables and all original clock wiring remain unchanged.
/// Non-power-of-two counts additionally require hostClock and aligned accessor
/// base-counter startup in abstract mode; production Xilinx mode resets these
/// accessor bases with hostReset to align them with the scheduler. Initialization files and annotation/symbol
/// targets needing renames reject atomically. Register-state banks are skipped.
mlir::LogicalResult threadFAME5Memories(circt::firrtl::FModuleOp module,
                                       unsigned threadCount,
                                       mlir::Value threadIndex,
                                       std::string &error,
                                       FAME5MemoryClockSemantics semantics =
                                           FAME5MemoryClockSemantics::Abstract);
} // namespace goldengate

#endif
