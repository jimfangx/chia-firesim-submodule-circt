// See LICENSE for license details.
#pragma once

#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "mlir/Support/LogicalResult.h"
#include <string>

namespace goldengate {
// Consume unused trigger annotations, or emit Scala-compatible accounting for
// credit/debit sources and node sinks. Sources are grouped
// by upstream input Clock leaf, including unconditional wire/node aliases and
// bundle/vector clock fields forwarded through internal instances. Each source
// domain needs both credits and debits. Local NEXT counts are synchronized into
// the annotated base clock before their full differences update global counts.
// Clock annotation targets may select typed bundle/vector leaves of top ports,
// wires or nodes; selected base/sink clocks remain the register clock operands.
// Sinks may use any proven input Clock leaf, independently of the accounting
// domains and other instances of the same definition. Each sink instance
// samples the shared enable on its own annotated local clock.
// Event/reset targets may select local UInt<1> bundle/vector leaves in the top
// or a descendant with unconditional instances through multiple parent definitions
// and unequal routes to top. Each absolute source instance resolves to its own
// proven top input Clock leaf; repeated definitions may span multiple domains.
// Field identity determines duplicate detection and flattened masked-event names.
// Repeated reset-masked sources each contribute through a distinct mask node
// and export. Mask identities follow credits before debits, preserving order
// within each kind even when both kinds mask the same target with different
// resets and coexist with unmasked annotations. All local mask names are
// allocated before source/relay export names; export collisions rename ports
// while routes retain the masked source identity. Unmasked sources share one
// export per target/absolute instance,
// counted once in each sourceType containing that target. Its last clock is
// selected after grouping credits before debits, before instance-path analysis.
// Distinct sources must have distinct flattened absolute instance/source
// identities. Ambiguous ancestor or underscore-separated paths fail before
// mutation, matching Scala TopWiring's nonunique port rejection. Flattened
// top export identities must also avoid existing top declarations and all
// planned top masks: Scala cannot reconstruct namespace-renamed top exports.
// Top bundle/vector ports and wire/node/register declarations reserve flattened
// leaf names, matching Scala normalization; aggregate container names disappear.
// Mask identities use the same normalized declaration namespace in every source
// module. Retained containers may rename native nodes without changing routes.
// Descendant exports also reserve normalized leaves and all local mask
// identities before choosing ports; SSA drivers survive renamed exports.
// Retained aggregate containers do not rename scalar exports: their distinct
// leaves replace the containers at LowerTypes while SSA connections stay bound.
// All source annotations are consumed.
// Requires: retained raw annotations; ground local UInt<1> node sinks in the
// top or a descendant with unconditional instances, including repeated ancestor
// instances; every ancestor route must reach top without cycles; descendant
// sink instance clock paths must each resolve to a top input Clock leaf;
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
