// See LICENSE for license details.
#pragma once
#include "goldengate/AutoILAAnalysis.h"

namespace goldengate {
struct WiredILAProbe {
  AutoILAProbe source;
  mlir::Value topPort;
  std::string topTarget;
};
// AutoILA's TopWiring step, after retained-target LowerTypes. Export one ground
// probe per instance path, with local declarations before ports and children.
// Expand shared modules once and rebuild every instance use, including unused
// parents. Allocate names per module and connect by SSA/port identity so name
// collisions cannot alias probes. Returned paths refer to live rebuilt instances.
// Debug annotations stay pending for wrapper/collateral construction. Invalid
// selections or hierarchy fail before mutation; append outputs only on success.
mlir::LogicalResult wireAutoILAProbesToTop(
    circt::firrtl::CircuitOp circuit,
    llvm::SmallVectorImpl<WiredILAProbe> &outputs, std::string &error);

struct ILAWrapperOptions {
  std::string outputBaseFilename;
  unsigned dataDepth = 1024;
  unsigned probeTriggers = 2;
};
// Convert targetutils debug annotations at the Golden Gate input boundary, or
// carry the circuit identity across host wrapper construction. Module/reference
// identities have already been handled by retained-target LowerTypes.
// Required input invariants: rawAnnotations retained; identity carry follows
// LowerTypes and host wrapping, conversion precedes both.
// Annotations consumed: public debug class or original debug circuit identity.
// Annotations produced: private debug class/current circuit identity.
// IR mutations: rawAnnotations only, committed after validation.
// Analyses required: none. Analyses preserved: all hardware analyses.
// Output invariants: unrelated annotations and module/reference names preserved.
mlir::LogicalResult prepareAutoILAAnnotations(
    circt::firrtl::CircuitOp circuit, bool internalizePublic,
    llvm::StringRef originalCircuit, std::string &error);
// Complete the hardware/collateral boundary after top wiring: replace the
// temporary top outputs by wrapper instance inputs, restore the old interface,
// emit inline Verilog and IP-generation annotations, and mark the Clock sink.
// The caller subsequently runs wireHostClock with retainSource=true. Routes
// must be the complete appended port suffix in probe order; their topPort
// values are invalid after success. Reject malformed routes before mutation.
mlir::LogicalResult attachAutoILAWrapper(
    circt::firrtl::CircuitOp circuit, llvm::ArrayRef<WiredILAProbe> routes,
    const ILAWrapperOptions &options, circt::firrtl::InstanceOp &wrapper,
    std::string &error);
// Final host AutoILA phase. Disabled/no-selection runs consume private debug
// and temporary TopWiring annotations without resolving their targets. Enabled
// runs create the wrapper and wire its Clock, retaining the source for the
// subsequent final HostClockWiring phase.
// Required input invariants: retained rawAnnotations; enabled selections are
// ground local targets in the current circuit, with a unique host Clock source.
// Annotations consumed: private debug, temporary TopWiring, HostClockSink.
// Annotations produced: ILA IP TCL and inline wrapper Verilog; source retained.
// IR mutations: probe exports, rebuilt instances, top wrapper and Clock routing.
// Analyses required: AutoILA selection and instance graph, built by callees.
// Analyses preserved: none when hardware changes; all for skipped runs.
// Output invariants: original top interface restored; every probe reaches its
// selected SSA value; no private debug/temporary TopWiring annotations remain.
mlir::LogicalResult runAutoILA(
    circt::firrtl::CircuitOp circuit, bool enabled,
    const ILAWrapperOptions &options, unsigned &probeCount, std::string &error);
} // namespace goldengate
