// See LICENSE for license details.
#pragma once
#include "mlir/IR/BuiltinOps.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace goldengate {
// Collapse the single-use host assembly wrapper chain after driver analysis,
// before XDC path resolution. Target/bridge/SRAM modules and boundaries owning
// inner symbols or explicit XDC references stay hierarchical.
// Assembly module names are GG*Wrapper, optionally followed by _<decimal>;
// only modules forwarding through a unique `sim` instance are candidates.
// Native CIRCT inlining maintains connections, annotations and inner symbols.
// Anonymous side effects remain unnamed; named declarations retain uniqueness.
// The serialized archive remains historical; inline blackboxes are attached
// before dead-module elimination and must not be replayed afterwards.
// Failure leaves the source IR unchanged. Repeated calls are a no-op.
mlir::LogicalResult normalizeHostHierarchy(mlir::ModuleOp source,
                                         llvm::StringRef outputFilename,
                                         unsigned &inlinedWrappers,
                                         std::string &error);
// Attach CIRCT's existing guarded initialization fragments to outlined memory
// modules before single-file export. Establish guarded MEM/REG randomization in
// the general header before its prolog is fixed, independent of module order.
// Preserve existing fragment/body order and user overrides. Unknown guards,
// defaults or unresolved symbols reject before mutation. Repeated calls are a no-op.
mlir::LogicalResult attachMemoryInitializationFragments(mlir::ModuleOp module,
                                                       std::string &error);
// Match CIRCT's generated memory initializer and share one 32-bit random
// draw across chunks and rows, as SFC's replicated initialization word does.
// Partial words may use a temporary padded to the next 32-bit boundary with
// a low-bit extraction; bounded low-bit chunk indices are also recognized.
// Unknown loop shapes, guards, bounds and extra consumers remain unchanged.
unsigned normalizeMemoryInitialization(mlir::ModuleOp module);
// Identities captured from explicitly marked native ClockBridge snapshot RegOps
// before lowering. Never infer source register identity from arbitrary SV names.
struct ClockSnapshotIdentity {
  std::string module;
  std::string reg;
};
// Restore the SFC allowed initialization states of each marked UInt64 snapshot:
// one captured RANDOM word duplicated in both halves. Unknown lowerings reject
// before mutation; other register initialization and live updates are untouched.
mlir::LogicalResult normalizeClockSnapshotInitialization(
    mlir::ModuleOp module, llvm::ArrayRef<ClockSnapshotIdentity> snapshots,
    unsigned &changed, std::string &error);
// Narrow a part-select base only when an SV initialization loop's strict
// constant upper bound proves that every executed base fits. Loop induction
// types/bounds/steps stay unchanged, including their termination bit.
unsigned normalizeInitializationIndices(mlir::ModuleOp module);
// Lower a clone of the transformed simulator through CIRCT's standard
// FIRRTL -> HW -> SV -> ExportVerilog pipeline. Retained Golden Gate JSON
// stays on the source IR; attached CIRCT annotations remain on the clone.
// Retained inline blackbox sources are attached to clone external modules and
// emitted beside the RTL with CIRCT's blackbox resource list for FireSim.
// Optional memory-origin
// collateral describes explicitly marked native packing and capture state only;
// it contains no cross-compiler equality or initial-state relation. Its hash binds
// the compiler-emitted RTL prefix, before any external resource append.
// A failed lowering never replaces an existing RTL output.
mlir::LogicalResult emitSimulatorRTL(mlir::ModuleOp source,
                                    llvm::StringRef inputFilename,
                                    llvm::StringRef outputFilename,
                                    std::string &error,
                                    llvm::StringRef memoryOriginsFilename = {});
} // namespace goldengate
