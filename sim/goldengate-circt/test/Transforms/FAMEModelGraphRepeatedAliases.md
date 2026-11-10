# Repeated memory/combinational definitions with exported clock aliases

The fixed `FAMEModelGraphRepeatedAliases.fir` and `.anno.json` input adds a second
instance of each virtual definition in `FAMEModelGraphMemory`. The original
compiler rejects it with `virtual model clock aliases require one definition
instance`; the unchanged SFC transform accepts it. The native graph rewrite now
passes the complete definition instance list to clock-output internalization.
That helper validates the full output-clock/instance cross product, including
all instance-graph uses, before changing any IR. It creates one local alias wire
in the shared definition, removes the corresponding result from every instance,
and removes each independent top alias output. Instance attributes, unrelated
results, and unrelated definitions remain intact. Model state is not merged.

Each alias must be wired directly and exclusively from its instance to a unique
unannotated, unsymbolized top Clock output. Model clock symbols with ground field
identity transfer to the local wire. Unsupported alias metadata or wiring,
omitted definition instances, and nested uses reject before mutation. This is a
bounded extension of the existing supported output-clock policy, not arbitrary
clock-net rewriting. The unit tests verify omitted-instance, annotated-alias and
unlisted-instance failures are atomic, positive helper metadata preservation,
and full graph construction in boundary and Pipe-transport modes.

The test has five model instances: one two-domain hub, two instances of the same
memory definition, and two instances of the same combinational definition. Each
memory instance contains independent 16x8 asynchronous-read and synchronous-read
old-RUW memories. Both virtual definitions export a clock alias used by channel
clock annotations. Each instance has independent input and output handshakes;
the hub drives rational clocks 1/2 and 1/3. No memory initialization or clearing
is introduced by the compiler or checker.

The existing `FAMEModelGraphMemoryOracle.scala` applies unchanged production SFC
`FAMETransform`, `AddRemainingFanoutAnnotations`, `DefineAbstractClockGate` and
`SimWrapper` from the immutable jar, retaining the exact prepared input and all
annotations. Actual Scala `VerilogCompiler` emits boundary RTL. The same
emitter-only annotation omission and firtool old-RUW lowering limitation
documented in `FAMEModelGraphMemory.md` apply. All 37 retained annotations and the
generated clock gate/name/MFMR contracts are compared independently without
omission. SFC sources and immutable goldens are unchanged.

`FAMEModelGraphMemoryCompare.scala` parses SFC and native output independently
and compares all model/top interfaces, complete top wiring for all five
instances, exact memory descriptors, every memory control/data/gated-clock
connection and local alias wires. `FAMEModelGraphRepeatedAliasesCompare.py`
runs the actual native CLI boundary RTL against actual SFC Scala RTL with
independent startup seeds 11/73 and the unchanged registered SFC gate. Over
16,384 cycles it applies independent stalls, stable held producer tokens,
rational held clock tokens and three host resets. All ready/valid controls,
clock enables and actual gated-clock edge schedules match strictly after the
first reset edge. Hub payload checks begin after explicit target reset warmup.

A separate ledger for each memory instance checks reads only after its own
address has been written and checks synchronous old-RUW collisions, masks,
write enables and persistence across host resets. Read-only monitors observe
actual target clock edges; they do not alter DUT state. The gate's unreset
startup enable can cause an initial edge, so only the monitor counter's initial
offset is normalized. Both combinational XOR results are checked independently.
Invalid memory payload corruption is ignored, whereas valid memory payload,
input-ready and cross-instance request-binding corruptions are rejected. These
are bounded two-state checks, not four-state or formal proofs.

Both the actual unchanged SFC SimWrapper and full native Pipe wrapper emit and
pass Verilator syntax checks. The SFC wrapper links through a named-port-only
`TargetBox` adapter to the actual Scala-emitted target; the adapter only maps the
two flattened clock vector indices to their original channel names. Runtime
checks cover the complete FAME boundary. Transported-wrapper runtime equivalence,
broader clock topologies and memory shapes, platform MMIO/driver ABI, separate
Jasper LEC/SEC, Linux/F2 feature execution and full feature acceptance remain
open. The independent unchanged SFC internal-ready-valid SimWrapper defect is
unaffected; this case uses scalar Pipe channels.
