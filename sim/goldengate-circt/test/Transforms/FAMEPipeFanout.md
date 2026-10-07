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
`0x5f3744639d41ea35`). The harness also reported those iteration 29 gates passed; the
UART-bearing SFC differential baseline remains pending.

This supplies discovery for the post-FAME boundary. Pre-FAME output selection
now deduplicates identical ordered model payloads on the same associated
clock, retains additional global branch names in `globalAliases`, and computes
dependencies once per producer. Clock-domain analysis validates every branch's
clock and supplies one assignment per producer. The top-port planner also
creates one source port per shared producer, checking binding, type, kind and
ordered source identity before reusing it. Source annotation renaming in
the compiler accounts for all branches referencing the same scalar top port.
`FAMEOutputSelectionTest.cpp` checks scalar and multiport sharing, unchanged
dependencies, reversed payload order, changed kind, partial overlap, and input
ownership; the scalar case also channelizes the selected producer through the
real FIRRTL rewrite. `FAMEChannelClockDomainsTest.cpp` checks that sharing adds
no domain FSM and a different-clock branch fails without mutation.
`FAMESharedProducerOracle.scala` executes unchanged production InferModelPorts
and FAMETransform host renames: shared scalar and aggregate payloads each have
one local output, both scalar branches resolve to the same model/top source,
and changed clocks, reversed order, and partial overlap fail. Scala's aggregate
representative name depends on map iteration; comparisons use payload identity.

Iteration 30 evidence is under the mutable generated-source directory's
`iteration30-shared-producer/`. Four focused CTests pass. Native and freshly
executed Scala scalar fixtures both report `PRODUCER printfB branches 2`.
The native compiler also ingests the oracle's exact
`scala/post-infer-model-ports.sfc.fir` and `.json`, plans one source port, and
channelizes it when requested by the second global branch name. Both branch
mappings, both output connection targets, the output model-group target, and
the single UInt8 model/top source ports match production
`scala/post-host-renames.sfc.json`. This comparison covers output FAME
annotations; other annotation classes are outside the standalone rewrite's
comparison. `producer-comparison.json` records the compared targets.
The fresh Rocket selection retains its 25 output names, payload sizes and
dependencies; it contains no shared output groups. Comparing its
`post-infer-model-ports.mlir` against immutable U250
`design/FireSim-generated.sv` matches all 17 input and 25 output clock FSM
assignments, including phase and reset. The four queue interfaces and 22
singleton valid/ready bindings also match that RTL. These recorded Rocket
boundaries do not exercise shared producers; the production Scala fixtures
provide that comparison. The fresh native compiler emits simulator RTL and
collateral with empty stderr. Iteration 30 harness gates passed CIRCT replacertl and Verilator smoke,
portable Spike, and Rocket suites (90,141 and 90,805 checks respectively).
The UART-bearing SFC differential baseline remains pending.
The final candidate is `compiler-candidate-final/`; its remaining-fanout
annotations are unchanged from iteration 29. Clock results are in
`golden-clock-comparison-final.stdout`, and queue results are in
`golden-interface-comparison.json`. Completed large MLIR boundaries are
gzip-compressed, retaining the two compared boundaries.

Iteration 31 collapses distinct scalar top aliases of one model producer into
one typed source interface through FIRRTL operations. It validates every old
port's annotation coverage, payload, direction, metadata, and direct wiring
before mutation; all instance-result uses must be those alias connects.
Unannotated outputs and other uses therefore fail rather than disappear.
Transferable wrapper DontTouch metadata and one payload symbol survive the
collapse. Multiple symbolic identities on the common payload fail atomically.
Both compiler output rewrite paths rename every old top reference and rename
the common model reference once. Source counts still follow branch count,
including repeated references to the same physical alias.

`FAMEOutputSelectionTest.cpp` covers a distinct physical alias, its unchanged
model data driver, one token interface, and transferred wrapper metadata.
Four additional cases reject unannotated aliases, extra model-result uses,
conflicting symbols, and extra top-alias uses without changing IR. Six focused
CTests pass, including coupled clock and queued-pipe traces in both orders.
The extended production Scala oracle confirms one InferModelPorts group and
both alias targets mapping to `model_printfB_source.bits`. The native compiler
ingests those exact FIRRTL/annotation files and channelizes the producer when
requested by the second branch. Both output connection classes, clocks,
latencies and source targets, plus the common model-group target, match
`scala/distinct-top-aliases/post-host-renames.sfc.json` in
`iteration31-output-aliases/`. `alias-comparison.json` records this comparison.
The probe compares production renames and model deduplication; it does not claim
a normalized full Scala FAME RTL result for this synthetic circuit. The
standalone native output rewrite leaves raw model DontTouch and input targets
outside that output-only comparison.

The fresh native Rocket candidate emits simulator RTL and collateral with
empty stderr. Its `post-infer-model-ports.mlir` matches all 17 input and 25
output clock assignments, including phase and reset, in immutable U250
`design/FireSim-generated.sv`. Its `post-fame-first-pipe-wrapper.mlir` matches
four queue interfaces and 22 singleton valid/ready bindings in the same
artifact. `golden-clock-comparison.stdout` and
`golden-interface-comparison.json` record those checks. The immutable compiler
annotation fixtures have no fanout annotation, and these recorded Rocket
boundaries do not exercise physical output alias collapse. Recorded UInt0
payloads also differ from current UInt3/UInt64 payloads; these comparisons are
not whole-design equivalence. Completed large MLIR files are compressed,
retaining the two compared boundaries. Iteration 31 manager gates await the
harness; no manager action was started by the implementation agent.

## Iteration 32: ordered multiport producer aliases

The multiport output rewrite now collapses distinct wrapper ports for each
producer leaf into one shared decoupled bundle. Retained source annotations
identify every alias. Their complete ordered SSA connections must match the
payload's field order, and each branch's global prefix removal must produce
the corresponding model leaf. Model bindings contain sorted physical indices;
these indices do not define payload order. The same preflight supplies payload
fields to the full compiler and standalone output target renames.

Each model leaf driver moves once. Every validated wrapper alias and direct
connect is removed; unrelated ports survive. Wrapper DontTouch metadata and
payload symbols transfer to field IDs. Seven new rejection cases cover missing
source coverage, reordered or partial branches, inconsistent field names,
extra model/top uses, and conflicting payload symbols. Rejections leave IR
unchanged. The positive fixture deliberately reverses payload versus physical
port order. All six focused CTests pass, including both clock-hub and queued
clock-hub orders.

The preserved Scala `InferModelPorts` and `FAMETransform.hostDecouplingRenames`
produce the `distinct-multiport-aliases` fixture under
`iteration32-multiport-aliases/scala/`. Its four wrapper outputs share one
ordered `data/valid` producer. SFC chooses the representative branch name for
an aggregate local group (here `right_`); both branches converge on
`model_right__source.bits.data/valid`. The native compiler ingests those exact
FIRRTL and annotation files. Its two output annotation classes, clocks,
latencies, four source targets, and two model leaf targets match
`post-host-renames.sfc.json`. Selecting either global branch produces the same
one physical token port and connect with the original leaf drivers.
`alias-comparison.json` records this output-only comparison. Raw input and
model DontTouch targets remain outside the standalone rewrite comparison;
this probe does not claim a normalized full Scala FAME RTL result.

The fresh Rocket candidate's `post-infer-model-ports.mlir` matches the immutable
U250 `design/FireSim-generated.sv` on 17 input and 25 output clock assignments,
including phase/reset behavior. `post-fame-first-pipe-wrapper.mlir` matches
four eight-port queue interfaces and 22 singleton valid/ready connections in
that same artifact. The recorded compiler annotation fixtures have no fanout
annotation and do not exercise multiport alias collapse. Their UInt0 queue
payloads differ from current UInt3/UInt64 payloads; these matches are scoped
boundary evidence, not whole-design equivalence. The production Scala alias
fixture supplies the missing differential case.

The prior iteration's harness passed CIRCT replacertl and the Verilator smoke,
portable suite (90,141 checks, `0x78194504c338c229`) and Rocket suite (90,805
checks, `0x5f3744639d41ea35`). The UART-bearing SFC workload baseline remains
pending. This iteration's manager gates remain owned by the harness.

Shared groups mixing channel kinds remain unsupported. The next small porting
step is to establish the SFC mixed-kind producer boundary and preserve each
branch's independent queue/handshake semantics. Mixed fanout also needs bridge
bindings to resolve each external queue output independently from the retained
upstream producer identity.
