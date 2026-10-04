// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Consume unused trigger annotations, or emit Scala-compatible accounting for
// distinct credit/debit sources and node sinks. Top-local sources are grouped
// by upstream input Clock leaf, including unconditional wire/node aliases and
// bundle/vector clock fields forwarded through internal instances. Each source
// domain needs both credits and debits. Local NEXT counts are synchronized into
// the annotated base clock before their full differences update global counts.
// Clock annotation targets may select typed bundle/vector leaves of top ports,
// wires or nodes; selected base/sink clocks remain the register clock operands.
// Event/reset targets may select local UInt<1> bundle/vector leaves in the top
// or a descendant with unconditional instances through multiple parent definitions
// and unequal routes to top. Every complete descendant source clock path resolves to the base leaf.
// Field identity determines duplicate detection and flattened masked-event names.
// Requires: retained raw annotations; ground local UInt<1> node sinks in the
// top or a descendant with unconditional instances, including repeated ancestor
// instances; every ancestor route must reach top without cycles; all sink
// instance clock paths must resolve to the same top base-clock leaf;
// each sink clock dominates its node declaration.
// Consumes: TriggerSource/TriggerSink and their internal annotation classes.
// Produces: no annotations; preserves unrelated annotation order and top IO.
// Descendant events are masked locally and exported through appended output
// ports from the deepest module upward. Relays preserve separate event exports
// for each sibling instance; every absolute instance contributes to accounting.
// Parent instances retain their original attributes and port connections. Sink enables
// flow downward through one appended input per definition and every sink
// instance; each instance retains independent sink synchronizer state in its
// declaring module on its annotated clock, including node aliases.
// Duplicate sink annotations select the last clock for that node; synchronizer
// names follow module declaration order, independently of annotation order.
// Mutates: source event/reset projections, source/relay output ports, sink/relay
// input ports and instances, top clock/event/reset projections, local/global counters,
// synchronizers and sink node inputs. Uses read-only field/hierarchy driver analysis before mutation;
// does not preserve hierarchy, top dataflow or dominance analyses after adding hardware.
// Output: six registers per accounting domain, two global registers, and one
// per sink, with Scala's 16-bit local and 32-bit global state and flattened
// root-leaf counter names.
// Unsupported hardware cases fail before mutation.
mlir::LogicalResult wireTriggers(
    circt::firrtl::CircuitOp circuit, unsigned &consumed,
    std::string &error);
} // namespace goldengate
