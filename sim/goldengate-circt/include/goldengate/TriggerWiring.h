// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Consume unused trigger annotations, or emit Scala-compatible local accounting
// for distinct credit/debit sources and node sinks on the circuit top base clock,
// including unconditional wire/node aliases and bundle/vector clock fields
// forwarded through internal instances to the same top input Clock leaf.
// Clock annotation targets may select typed bundle/vector leaves of top ports,
// wires or nodes; selected base/sink clocks remain the register clock operands.
// Requires: retained raw annotations; ground top-local UInt<1> event/reset
// references and node sinks; each sink clock dominates its node declaration.
// Consumes: TriggerSource/TriggerSink and their internal annotation classes.
// Produces: no annotations; preserves unrelated annotation order and module IO.
// Mutates: top clock projections, local/global counters, synchronizers and sink
// node inputs. Uses read-only field/hierarchy driver analysis before mutation;
// does not preserve top dataflow or dominance analyses after adding hardware.
// Output: nine registers for one accounting domain and one sink, with Scala's
// 16-bit local and 32-bit global state and flattened root-leaf counter names.
// Unsupported hardware cases fail before mutation.
mlir::LogicalResult wireTriggers(
    circt::firrtl::CircuitOp circuit, unsigned &consumed,
    std::string &error);
} // namespace goldengate
