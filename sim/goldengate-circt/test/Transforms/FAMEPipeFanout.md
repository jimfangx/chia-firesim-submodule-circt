# PipeChannel fanout and model loopbacks

`addFAMEPipeWrapper` consumes `FAMEChannelFanoutAnnotation.channelNames` in
annotation order. The first name selects the shared bridge input. Each member
keeps its own queue, latency, and target sink; secondary bridge inputs disappear
from the wrapper interface. The emitted FIRRTL uses the production Scala
`SimWrapper.genPipeChannel` enqueue rule:

```
source.ready = AND(queue[i].in.ready)
queue[i].in.valid = source.valid AND AND(queue[j].in.ready, j != i)
queue[i].in.bits = source.bits
```

Excluding the receiving queue's own ready preserves DecoupledHelper behavior.
The physical enqueue handshake is identical for every queue. Removing secondary
ports also requires clock and ReadyValid wrapper connections to resolve ports
by name, type, and direction rather than their old target indices. Annotation
activation retains secondary sink references on the inner target.

Supported groups have identical complete payload types. Bridge-sourced groups
retain distinct model sinks. Target-sourced groups share exactly one producer
port and have model loopback sinks plus at most one bridge sink. Each loopback
has one source and one sink. Wrapper IO omits internal sinks and, for internal-only
groups, the producer. The shared source annotations retain the inner target
identity even when one branch exposes a wrapper output: that output is downstream
of its queue. A standalone model loopback uses the same queue construction.

Validation precedes mutation and rejects empty groups, unknown names, duplicate
or overlapping membership, mismatched endpoint types, repeated sinks, different
target producer ports, split or absent fanout groups for shared producers, and
multiple bridge outputs sharing one source. Scala `ChannelizedWrapperIO`
deduplicates identical source targets on wrapper IO; exposing multiple independent
bridge outputs there would diverge from the oracle.

## Differential fixture

`FAMEPipeFanoutTest` constructs a three-sink UInt16 boundary with latencies
0, 1, 0, invokes the production builders, verifies the resulting CIRCT circuit,
and executes its emitted FIRRTL graph. `FAMEPipeFanoutOracle.scala` elaborates
three production `midas.core.PipeChannel` instances and the Scala wrapper's
actual `DecoupledHelper` rule, then executes their emitted Low FIRRTL.

Both interpreters retain separate queue instance state and use independent FIFO
scoreboards. The 4,096-cycle stimulus holds pending input payloads stable under
backpressure, stalls consumers independently, and resets twice. Traces compare
source readiness, all three output valid bits and valid payloads, and all enqueue
valid/ready bits. Invalid payloads are canonicalized to zero. Every queue reaches
depth two; the latency-one queue inserts one zero seed after each reset epoch.
The native fixture additionally verifies shifted clock-port identity and retained
annotation targets. `FAMEReadyValidChannelTest` checks ReadyValid connections in
a wrapper whose secondary fanout port was removed.

Artifacts are written under the generated Rocket directory's
`iteration27-pipe-fanout`: native and Scala traces, exported native MLIR and RTL,
Scala `queues.sfc.fir`, test/build logs, and the fresh Rocket compiler candidate.

The native and Scala traces match all eight numeric fields on all 4,096 cycles
with zero mismatches. Their normalized TRACE SHA256 is
`f9ffd07a4da90a748a6ad729480e93ef4efd07cf990636e9dc5c8ff8ad128c01`.
Both execute 2,335 input transfers and 1,379 stalled input cycles; queue readiness
differs on 1,399 cycles. Ten focused native CTests pass, including both existing
queued clock-hub orders. The exported fanout circuit lowers through firtool to
SystemVerilog. The local Rocket compiler candidate also emits simulator RTL,
annotations, headers, and XDC successfully; its stderr is empty. FireSim manager
verification remains owned by the external harness.

## Recorded golden boundary

The immutable U250 build-tree reference
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`
provides existing singleton PipeChannel interfaces and enqueue wiring. The
recorded Rocket fixtures contain no fanout annotation: they cannot establish
fanout differential parity. Use the executed production Scala fixture for that
behavior and compare singleton queue interfaces against the fresh candidate's
`post-fame-first-pipe-wrapper.mlir`.

The fresh candidate matches all eight port names, directions, and widths for
golden `PipeChannel`, `PipeChannel_1`, `PipeChannel_2`, and `PipeChannel_7`
against native `GGFAMEPipe32`, `GGFAMEPipe1_L0`, `GGFAMEPipe1`, and
`GGFAMEPipe40`. All 22 golden singleton enqueue-valid and source-ready bindings
also match after normalizing target/bridge instance prefixes. The exact artifact
paths and compared bindings are recorded in `golden-interface-comparison.json`.

Recorded zero-width payloads and current Rocket UInt3/UInt64 payloads remain a
preexisting fixture difference. Neither this comparison nor the synthetic
fixture establishes whole-design RTL equivalence or completion of the port.

## Target-sourced differential extension (iteration 28)

`FAMEPipeFanoutTest` now also executes a three-branch target source: fork0 drives
one external bridge output, while fork1/fork2 drive retained model sinks. Latencies
are again 0, 1, 0. Tests inspect actual target-source readiness and actual bridge
and model output ports. The existing bridge trace, new target trace, and executed
Scala queue trace match all eight fields on all 4,096 cycles with the SHA256 above.
Each executes 2,335 transfers, 1,379 source stalls, and two latency-one reset seeds.

`FAMEPipeFanoutBoundaryOracle.scala` elaborates production `midas.core.SimWrapper`
for mixed fanout, internal-only fanout, and a single model loopback. It reverses
fanout name order to exercise primary-name independence for target sources. The
mixed native wrapper matches 16 normalized production Scala branch/source
connections: upstream bits/valid/ready, every queue enqueue, each sink's dequeue
ready, and the bridge/model valid/payload outputs. Native tests also verify hidden
internal ports, shifted clock ports, preserved source/sink annotation targets,
and atomic rejection of six malformed target fanouts (in addition to six malformed
bridge groups). Eleven focused CTests pass; after adding the last rejection cases,
both fanout CTests pass again. Native mixed fanout lowers through firtool to RTL.

Artifacts are in the generated Rocket directory's `iteration28-target-pipe-fanout`:
`target-fanout.mlir`, `target-fanout.sv`, the three traces, `scala-boundary/*.sfc.fir`,
`boundary-comparison.json`, `trace-comparison.json`, and the fresh compiler output.
The compiler emits Rocket simulator RTL and collateral with empty stderr. The
same immutable U250 `design/FireSim-generated.sv` comparison matches all four
queue ABIs and 22 singleton enqueue-valid/source-ready bindings, recorded in
`golden-interface-comparison.json`. The immutable recorded fixtures still contain
no fanout annotation, so this does not establish recorded mixed-fanout parity.

At the iteration 28 boundary the full compiler did not synthesize Scala's
`AddRemainingFanoutAnnotations` for shared model sources. Bridge token engines require
boundary annotation endpoints to resolve to the active wrapper, so binding a
mixed group to a native UART/TSI/Print bridge needs explicit external endpoint
resolution that preserves the common upstream source identity. The tested change
implements the annotated post-FAME queue/wrapper boundary; it does not establish
end-to-end mixed-fanout integration. The next smallest step is generating and
validating target-source fanout annotations after native FAME source deduplication,
then teaching bridge bindings to resolve the queue output separately.

The harness-owned iteration 27 gates passed (portable suite 90,141 checks,
`0x78194504c338c229`; Rocket suite 90,805 checks, `0x5f3744639d41ea35`). Current
changes await the next manager verification. UART-bearing SFC differential
baseline remains pending; neither these tests nor those gates complete the port.

## Remaining model fanout discovery (iteration 29)

`RemainingFanout.cpp` now synthesizes `FAMEChannelFanoutAnnotation` from live
post-FAME source identities. The compiler invokes it after all output channel
and control rewrites, before queue construction, and emits
`post-fame-remaining-fanout.json`. Its key is an ordered sequence of CIRCT
module/port/field identities. It ignores channel latency, clock, and sinks,
deduplicates names within each group, and preserves first group/name occurrence.
All existing annotations, including bridge fanouts, remain in order. Like the
production Scala transform, this is an append-only, one-shot step; a second
execution appends the groups again. Invalid local output references fail before
mutation. Hierarchical or internal declaration sources remain unsupported.

`RemainingFanoutTest.cpp` and `RemainingFanoutOracle.scala` execute native and
unchanged production Scala discovery with interleaved groups, different latencies
and clocks, loopback sinks, repeated channel names, non-Pipe channels, and bridge
sources. Both produce exactly `a0,a1`, `b0,b1`, `ordered0,ordered1`; reversed source
order forms a distinct singleton. Native tests additionally reject stale,
foreign-circuit, and input-port targets atomically. The target-source behavioral
fixture now removes its supplied fanout annotation and runs discovery before
production queue/wrapper construction.

Evidence is in the generated Rocket directory's `iteration29-remaining-fanout`.
The new Rocket annotation boundary preserves all 104 input FAME annotations,
including seven bridge fanout annotations, and adds no model fanout groups for
this singleton-source input (`rocket-fanout-preservation.json`). The recorded
immutable U250 `design/FireSim-generated.sv` again matches the candidate's four
eight-port queue interfaces and 22 singleton enqueue-valid/source-ready bindings
(`golden-interface-comparison.json`). The recorded primary fixture's annotation
files contain no FAME fanout annotation and cannot prove shared-source behavior.

The generated target fanout trace, native bridge fanout trace, and freshly
executed Scala queue trace match all eight numeric fields on all 4,096 cycles,
with zero mismatches and TRACE SHA256
`f9ffd07a4da90a748a6ad729480e93ef4efd07cf990636e9dc5c8ff8ad128c01`.
The target wrapper matches all 16 normalized production Scala `SimWrapper`
branch/source connections. Twelve focused native CTests pass. The exported
target wrapper lowers through firtool to SystemVerilog; a fresh native Rocket
compiler run emits simulator RTL and collateral with empty stderr. This local
compiler execution does not run any manager verification gate. Large completed
candidate IR boundaries are gzip-compressed, retaining the compared wrapper.

The harness-owned iteration 28 gates passed the smoke, portable suite (90,141
checks, `0x78194504c338c229`), and Rocket suite (90,805 checks,
`0x5f3744639d41ea35`). Iteration 29 changes await harness verification, and the
UART-bearing SFC differential baseline remains pending.

This supplies discovery for the post-FAME boundary. Pre-FAME shared producers
still fail native output selection and exclusive-output rewrite checks. The
next smallest step is deduplicating equal output groups during selection and
channelizing their common producer once, renaming every branch to that source.
Mixed fanout also still needs bridge bindings to resolve each external queue
output independently from the retained upstream producer identity.
