# Bridge-sourced PipeChannel fanout

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

Supported groups have bridge-sourced, single-endpoint channels and identical
payload types. Validation precedes mutation and rejects empty groups, unknown
names, duplicate or overlapping membership, mismatched types, and target-sourced
groups. Target-sourced fanout and loopback channels remain future work.

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
The next small step is target-sourced fanout with independent external sinks.
