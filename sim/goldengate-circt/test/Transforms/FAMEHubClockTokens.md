# Bundled hub clock leaves

`FAMETransform.scala` replaces each clock port with the corresponding clock
channel payload leaf. A one-port channel uses `sink.bits`; a multiport channel
uses `sink.bits.<port suffix>`. Each domain independently samples that raw
leaf into a host-clocked, reset-to-zero enable register and gates its target
state with `enabled & targetCycleFinishing & ~hostReset`.

Native `addFAMEClockGate` accepts the passive Clock leaf below an input's
`bits` field. CIRCT `FieldRef` identity matches target reads across shared and
separately constructed selector trees. It preserves the supplied raw token
SSA value, replaces other reads of that exact leaf, and leaves sibling clocks
alone. Output, flipped, non-payload and foreign-model selectors, or a domain
with only sibling replacement reads, fail before gate construction mutates IR.

## Executable comparisons

`FAMEHubClockOracle.scala` invokes the preserved Scala `FAMETransform.execute`
on a two-clock hub. It checks the emitted Clock payload, host-clock/reset-zero
enable registers, gate inputs and target register clock substitutions. It
evaluates the emitted enable mux and CE expressions for all 64 assignments of
the two raw tokens, two buffered enables, finishing and host reset.

`FAMEClockGateIdentityTest.cpp --hub-observations` evaluates the corresponding
native operations. Its 64 `HUB` rows match the Scala rows exactly: 128 enable
mux evaluations and 128 gate CE evaluations. Four target registers test
separate and shared selector trees. Two domain XDC constraints retain separate
gate output pins through serialization and BUFGCE specialization, with setup/
hold MFMR values 2/1 and 3/2 and divide-by-one clocks. The fixture intentionally
leaves the finishing signal as an input to the clock construction boundary;
it does not establish two-domain data-channel FSM equivalence.

The immutable U250 boundary used for the one-clock compatibility comparison is:

```text
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.implementation.xdc
```

Pass these files and the mutable actual Rocket
`circt-ingestion/post-fame-twenty-fifth-output-control.mlir` to the identity
test. It regenerates the gate through the changed native helper, preserves
target clock uses and the raw enable token, checks eight CE assignments, and
matches all three generated-clock XDC commands against SFC. As in the prior
comparison, the recorded final wrapper prefix and model instance name are
supplied for this pre-wrapper boundary.

Local evidence is under the mutable generated-source tree in
`iteration12-bundled-clock/`: `sfc-two-clock.fir`, `sfc-two-clock.log`,
`sfc-observations.log`, `native-observations.log`, `rocket-boundary.log`,
`build-final.log` and `tests-final.log`. The recorded Rocket fixture has one
target clock; the additional two-domain oracle is produced by executing the
preserved compiler, and is not an immutable Rocket fixture.
The compiler and relevant native test binaries build successfully; 19 focused
CTest checks pass, followed by three mixed-clock checks in `tests-mixed.log`.

## Ordered domain analysis

`analyzeFAMEHubClockDomains` captures each model clock, connected top clock,
payload field and rational clock/MFMR record before scalar port erasure. It
follows the annotation sequence used by Scala `portsByInputChannel` and
`clockMetadata`, rather than the sorted set in `ModelChannelBinding`. It
rejects inconsistent local/global order, repeated ports, non-Clock ports,
missing connections and invalid metadata without changing IR. Repeated exact
RationalClock keys retain Scala's last-MFMR map semantics.

The baseline compiler now uses these records for clock identity and constraint
selection. `FAMEHubClockDomainsTest.cpp` also constructs both native enable/
gate pairs after erasing the scalar ports, with reversed physical declarations
and both annotation orders. Its four `DOMAIN` rows match the actual Scala
oracle's emitted gate-constraint associations exactly; 13 malformed cases are
rejected in each order. The native gate constraints and target state clocks
remain attached to the correct domains.

The actual Rocket `circt-ingestion/post-fame-host-control.mlir` yields one
scalar domain, `clockBridge_clocks_0`, with ratio 1/1 and MFMR 1. Its metadata
matches the immutable ClockBridgeChannel in:

```text
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
```

Iteration 13 evidence is in `iteration13-clock-domains/`: Scala and native
normal/reversed domain observations, `rocket-domains.log`,
`rocket-gate-boundary.log`, successful build logs and `tests-final.log` (24
focused checks). The gate comparison again matches the immutable U250 RTL
contract and all three generated-clock XDC commands listed above. These local
tests do not start the harness-owned manager verification gates.

## Data-channel clock assignments

`analyzeFAMEChannelClockDomains` resolves each bound data channel's associated
model clock alias before any channelization erases ports. It uses the native
FIRRTL field/hierarchy connectivity analysis and captures global/local channel
names, direction and original hub clock identity. As in Scala `genMetadata`,
the alias must reach exactly one other model port, a scalar input Clock in the
hub. UInt mux selectors, intermediate output aliases, undriven clocks, missing
hub sources and stale domain identities fail without mutating IR. A hub data
channel without an associated clock is rejected; virtual-clock behavior stays
in its existing separate path.

The actual baseline compiler consumes these assignments when selecting each
input's raw-token enable and each output's tagged buffered enable. It no longer
assumes a data channel belongs to the first hub domain. The synthetic native
test consumes the captured names after scalar clock/alias ports are gone,
including reversed physical clock/payload declarations. Its 256 `FIRED` rows
(1,024 transitions) match the preserved Scala oracle in both clock annotation
orders. Each generated input fired register resets to one; each output resets
to zero. Cases with different raw tokens and buffered enables check the token
phase as well as domain identity.

The actual Rocket `post-fame-host-control.mlir` yields 42 channel assignments.
The native comparison resolves the immutable U250 RTL's named transition wires
and matches all 17 input raw-token reset/transition equations and all 25 output
buffered-enable reset/transition equations in `design/FireSim-generated.sv`
listed above. This fixture provides only one domain; the two-domain differential
comes from the additional executable Scala fixture, not the immutable Rocket.

Iteration 14 evidence is under `iteration14-channel-clocks/`: successful native
build logs, `sfc-two-clock.fir`, `sfc-reversed.fir`, Scala/native `FIRED` rows,
`rocket-domains-final.log` and `tests-final.log` (27 focused CTest checks).
Earlier local comparison attempts exposed a test parser that did not follow
SFC's intermediate input transition wires; the final comparison resolves those
wires and checks host reset separately. No manager gates were launched here.

## Ordered clock payload rewriting

The actual compiler driver now channelizes every captured hub domain through
`rewriteFAMEHubClockChannel`. The helper preflights the original port identities,
payload field order, retained target order and unique annotation occurrences before erasing
ports. It transfers ChannelConnection sinks and ChannelPorts ports to scalar
`.bits` or ordered `.bits.<field>` targets, and optionally transfers private FPGA
debug selections in both ReferenceTarget and ComponentName spelling. Other
annotation members and archive order remain intact. The driver creates each raw
Clock leaf and UInt flag, then each buffered enable, gate and constraint using
the captured original clock identity.

`FAMEHubClockDomainsTest.cpp` runs this same helper with reversed physical clock
ports and both annotation orders. Four emitted payload targets, two domain
metadata records and 64 HUB rows (256 enable/CE evaluations) match the preserved
Scala transform's actual RenameMap and hardware expressions in each order.
Seven malformed rewrite cases per order leave IR unchanged, in addition to the
existing analysis rejections. Each target state register uses its own gate.

Iteration 15 evidence is under `iteration15-hub-payload/`: native and Scala
normal/reversed observations, `rocket-payload.log`, `rocket-gate-xdc.log`, native
build logs and `tests-final.log` (27 focused CTest checks). Applying the new
helper to actual Rocket `post-fame-host-control.mlir` retains the scalar Clock
payload and matches ratio/MFMR against the immutable primary annotation above.
The corresponding scalar payload exists in the immutable U250
`design/FireSim-generated.sv`; the gate comparison also matches that RTL's CE
contract and all three commands in `design/FireSim-generated.implementation.xdc`.

## Associated clock references at hub channelization

`rewriteFAMEHubClockChannel` also transfers
`FAMEChannelConnectionAnnotation.clock` and
`FAMEChannelPortsAnnotation.clockPort` when they identify an erased hub input.
They receive the same scalar `.bits` or ordered `.bits.<field>` replacement as
that input. Surviving output aliases remain unchanged. Shared associated
references do not count toward the clock channel's unique endpoint occurrences.
This follows `FAMETransform.hostDecouplingRenames` and the explicit `update`
methods in `midas/passes/fame/Annotations.scala`.

The new native regression failed on the previous production helper with
`associated clock still identifies an erased hub port`. In each of the normal
and reversed two-clock orders it now checks four shared global/local reference
pairs, exact preservation of other annotation members, and resolution to live
Clock leaves. The existing 13 analysis and seven rewrite rejection cases still
leave IR unchanged. `FAMEHubClockOracle.scala` invokes the annotation update
APIs against the actual Scala transform's RenameMap: its two associated-reference
pairs, four endpoints, two domain records and 64 enable/gate rows match native
observations in both orders. The direct clock associations are transfer probes;
the complete Scala fixture uses output aliases for local domain analysis. These
tests do not establish full two-clock simulator support or a coupled producer/
hub state trajectory.

The captured Rocket `circt-ingestion/post-fame-host-control.mlir` is rewritten
through the changed helper. All 84 associated references (42 global and 42 local)
retain their original output aliases and resolve to Clock ports. The candidate's
ordered clock name, ratio 1/1 and MFMR 1 match the immutable primary
`firechip.chip.FireSim.FireSimRocketConfig.anno.json` above; its scalar clock
payload is also present in the immutable U250 `design/FireSim-generated.sv`
above. The recorded Rocket has one domain and does not exercise the direct-input
reference defect.

Iteration 19 evidence is under the mutable generated-source directory
`iteration19-hub-clock-references/`: `regression-before.log`, `build.log`,
`tests.log`, `scala-{normal,reversed}.log`, `native-{normal,reversed}.log`,
`sfc-{two-clock,reversed}.fir`, `rocket-comparison.log` and `comparison.json`.
The actual compiler and three relevant native test binaries build successfully;
four focused CTest checks pass. No manager gates were launched by this change.

## Remaining work

The hub FAME boundary now uses all ordered clock domains. The active compiler
calls `addClockBridge`, which maps ordered rational lanes, their source/sink
targets and the fastest-clock counter. The actual Scala bridge comparison
is recorded in [RationalClockBridge.md](RationalClockBridge.md); the inner
scheduler comparison is in
[RationalClockTokenGenerator.md](RationalClockTokenGenerator.md). Next couple
the rational producer to the multiclock FAME hub under independent data-channel
stalls. The baseline's data/bridge configuration remains Rocket specific.
Manager gates remain harness-owned; FAME-5 and the SFC UART-bearing differential
remain incomplete.
