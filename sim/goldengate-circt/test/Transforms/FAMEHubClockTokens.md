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

## Coupled rational producer and hub completion

`FAMEHubClockCoupledTest.cpp` constructs the actual native rational producer
and connects its ready signal to a two-domain hub built with the production
FAME enable, gate, fired-state, input-ready, output-valid and finishing helpers.
The two target registers advance by 1 and 3 only on their actual gate CE.
The producer lanes have ratios 1/2 and 1/3; reversing the ClockRecord field
order reverses their physical domain assignment. Independent sticky input
valids, independent output-ready stalls, and a mid-run host reset exercise
early firing, disabled domains and producer backpressure.

This regression failed before the production fix with
`missing Clock-typed target clock sink bridge_clocks`. `rewriteFAMEFinishing`
now accepts the nonempty passive record of scalar Clock leaves produced by
`HasModelPort`, applying the record's single valid/ready handshake once.
Data-only, empty, mixed, flipped, nested and vector payloads, and a ClockRecord
wrongly listed as a data input, reject before mutation. Seven new atomic
rejection cases check that contract.

`FAMEHubClockCoupledOracle.scala` executes the preserved Scala FAME transform
and elaborates the actual Scala rational generator. Both interpreters evaluate
the emitted operations, updating host registers simultaneously and target
registers only on emitted gate CE. All 8,192 rows in each clock ordering match
exactly: producer tokens, completion, buffered enables, fired state,
input-ready, output-valid, gate CE and both target states. Normal order gives
705 non-reset completions and 528/352 target edges; reversed order gives
653 completions and 326/489 edges. Both runs include over 7,400 stalled host
cycles. Unreset target state is seeded to zero in both interpreters; this is
an IR differential, not a physical initialization guarantee or a multiclock
manager simulation. The native fixture begins at the channelized model
boundary; prior hub tests cover its annotation and port conversion.

The modified finishing helper is also reapplied to the captured Rocket
`circt-ingestion/post-fame-twenty-fifth-output-control.mlir`. Its clock-ready
predicate matches the normalized Boolean tree in the immutable U250
`design/FireSim-generated.sv` named above: exactly 17 input-valid terms and
25 output `(fired | (ready & valid))` terms. Completion matches with the
recorded constant-valid scalar clock. The immutable Rocket has one clock and
does not exercise the ClockRecord rejection.

Iteration 20 evidence is in the mutable generated-source directory
`iteration20-coupled-hub/`: `native-{normal,reversed}.{mlir,trace,sv}`,
`scala-{normal,reversed}.trace`, each Scala directory's `hub.sfc.fir` and
`producer.sfc.fir`, `rocket-finishing.log`, `comparison.json` and `tests.log`.
The compiler and both relevant test binaries build; 17 focused CTest checks
pass. CIRCT firtool lowers both coupled native circuits to Verilog successfully.
The build logs are `goldengate-circt-build/iteration20-build{,-final}.log`.

To reproduce, build `goldengate-circt`,
`goldengate-fame-hub-clock-coupled-test` and
`goldengate-fame-fired-state-test` in the configured CMake directory. Run the
coupled binary with `--normal <output.mlir>` and `--reversed <output.mlir>`;
stdout is the trace. Compile the Scala oracle using the existing midas compile
dependency classpath and Scala 2.13.10 compiler, then run it with an output
directory and, for reversed order, a second `--reversed` argument. Compare
every trace row. The fired-state binary takes the captured Rocket MLIR as its
sole argument and prints the reconstructed `COMPLETION` and `CLOCK_READY`
expressions for structural comparison with SFC RTL. Source the mandatory
FireSim environment before these local compiler commands. No manager gates
were launched.

## Input-dependent payload differential (iteration 21)

The coupled fixture now feeds sticky 16-bit payloads into each target domain.
Each gated target register accumulates its current input modulo 65536, and
each output combinationally adds that register and input. The native pass uses
the corresponding input-valid dependency for each output, matching the actual
Scala `FAMETransform` result. Input payloads stay fixed until accepted; both
interpreters independently accumulate the expected target state on emitted
gate CE. The native test also checks that valid output payloads remain stable
under backpressure, including long independent stalls and host resets.

This work exposed a production validation gap: `rewriteFAMEOutputValids`
accepted a flipped `valid` field on a source or dependency sink, even though
Scala `HasModelPort` constructs passive `UInt<1>` valids. It could drive a
peer-owned source valid or read the wrong-direction dependency. The helper
now rejects both cases before changing any channel. Two fixtures put a valid
output first and a malformed output/dependency second, checking that the
complete module remains unchanged. Before the fix the regression failed with
`flipped valid field accepted or partially rewritten`.

All 8192 trace rows match the preserved Scala oracle in each clock ordering,
including both target states and both output payloads. Normal order has 600
non-reset completions, 7587 stalled host cycles and 449/300 target edges;
reversed order has 575 completions, 7612 stalls and 287/430 edges. The trace
hashes are `0d6d75877b969a9f85a257631bb8c29083ff312a1c0a6c827286590b5869c8d4`
and `8b62879eceff156a37298af0d8556a6eccad199cd566a9c6e34258ac18b62be2`.
Both native circuits pass FIRRTL verification and firtool Verilog lowering.
Ten focused CTest checks pass; the two coupled checks pass again after adding
the explicit blocked-output assertions. No manager gates were launched.

For the immutable comparison, the fixed output-valid helper is reapplied to
`circt-ingestion/post-fame-twenty-fifth-output-control.mlir`, using the captured
pre-channelization `fame-output-selection.json` dependency analysis. All 25
rewritten predicates match the FireSim module's assignments in the immutable
U250 `design/FireSim-generated.sv` named above. The comparison preserves exact
channel/register names, negation, and operand multiplicity while normalizing
conjunction association/order. This establishes Rocket compatibility; the
immutable single-clock fixture does not supply a multiclock payload oracle.

Evidence is in the mutable generated-source `iteration21-payload-oracle/`:
`native-{normal,reversed}.{mlir,trace,sv}`, `scala-{normal,reversed}.trace`,
the Scala `hub.sfc.fir`/`producer.sfc.fir` outputs, `rocket-output-valids.log`,
`comparison.json`, `tests.log` and `tests-final-payload.log`. Build logs are
`goldengate-circt-build/iteration21-{build,boundary-build,final-build}.log`.
Run the fired-state boundary binary as
`--output-valids <candidate.mlir> <fame-output-selection.json>` to reconstruct
the predicates. The coupled/Scala invocation remains as described above.

## Annotation-selected data channelization (iteration 22)

`analyzeFAMEDataSelection` snapshots both input and output data channels before
port mutation, retaining their global/local identities, channel kind, payload
field count and graph-derived output dependencies. Explicit target-clock
channels are excluded. Duplicate local input names now fail without mutation;
the selection test also checks connection annotation order, heterogeneous
channel kinds and isolation from another model's inputs. The existing output
selection API delegates to this shared analysis.

The active `--compile-baseline` path uses this snapshot to channelize every
selected data input. It previously selected exactly 17 inputs with hardcoded
Rocket channel names, payload counts and ordinal-specific conditions. Each
iteration now resolves the selected global identity against fresh annotations
and hierarchy bindings, checking its local identity and shape before the
existing CIRCT rewrite. Input controls and cycle completion use the resulting
complete input list. Historical intermediate filenames remain for the
17-input Rocket case; they no longer select channels.

The coupled fixture starts with scalar data ports and their actual arithmetic
drivers. It discovers channel dependencies through the production analysis,
then calls the real input/output port rewrites with fresh bindings after each
mutation. Its interpreter projects the resulting aggregate connects, including
flipped ready fields. Both 8192-cycle traces exactly match fresh executions of
the Scala `FAMETransform`/rational-token oracle, with the same hashes and payload
coverage recorded for iteration 21. Both native circuits lower to Verilog.
The ClockRecord interface and domain-to-lane assignment are still constructed
in the fixture; data channelization and dependencies are no longer supplied
by hand.

A local invocation of the native compiler on the current harness handoff emits
simulator RTL and collateral successfully. Its `fame-input-selection.json`
matches the 17 annotation-selected inputs in the saved pre-FAME boundary.
Compared with the immutable U250 `design/FireSim-generated.sv` named above,
the emitted FireSim module matches all 17 input and 25 output handshake
identities, all 84 data-handshake directions/widths and all 72 payload ports
present in SFC RTL. SFC removes some unused payload fields that CIRCT retains;
no byte-for-byte RTL claim is made. All 25 output-valid predicates and the
clock-ready predicate match after expanding wire aliases and normalizing
Boolean conjunction/disjunction order. Completion also matches when the
explicit native clock-valid input is set to the bridge's constant valid token,
which SFC folds away. The recreated 42 fired-register contracts pass 2688
reset/completion/handshake cases on this new candidate.

The comparison uses the current harness handoff as candidate input and reads
the immutable SFC RTL as reference. An initial direct import of the immutable
raw `*.fir`/`*.anno.json` pair was rejected for duplicate `BridgeAnnotation`s
on `PeekPokeBridge`; the fixture was left untouched. This raw-import issue
does not establish compatibility with that original annotation container.

Evidence is in the mutable generated-source
`iteration22-channelization-oracle/`: native/Scala traces and FIRRTL outputs,
`compiler-candidate/`, `compiler.{stdout,stderr}.log`,
`rocket-data-selection.log`, `rocket-finishing.log`, `rocket-output-valids.log`,
`comparison.json` and `tests-final.log`. Eleven focused CTest checks pass after
the final build. Build logs are `goldengate-circt-build/iteration22-*.log`.
The selection boundary command is
`goldengate-fame-output-selection-test --data-selection <pre-FAME.mlir> FireSim`.
No manager gates were launched.

## Shared production hub clock controls (iteration 23)

`constructFAMEHubClockControls` now owns the active compiler's raw Clock
selectors, UInt token flags, buffered enable registers, abstract gates and
generated-clock constraint attributes. It checks the complete scalar Clock or
passive ClockRecord payload against ordered domain metadata before creating
any controls. The result associates raw input and buffered output flags with
the original model clock identity. Malformed shape/order metadata fails without
mutation; later construction failures do not promise rollback. The combined
driver boundary is `post-fame-clock-gate.mlir`; the former enable-only dump is
no longer emitted.

The coupled fixture now starts with scalar clock ports and output aliases.
Production domain and associated-clock analyses capture identities before
`rewriteFAMEHubClockChannel` constructs the ClockRecord. Data fired controls
select the shared helper's raw/buffered flags through these captured identities.
Output aliases are internalized and checked against their actual gate outputs.
Five malformed control metadata cases per annotation order leave IR unchanged.
The producer schedule and lane fields come from the ordered domain records.
For local producer integration the wrapper clock input becomes a same-type
internal wire, retaining the channel rewrite's aggregate connect and flipped
ready flow. This does not yet exercise the complete `addClockBridge` mapping.

Both 8192-cycle native traces exactly match fresh executions of the preserved
Scala oracle: normal SHA256
`0d6d75877b969a9f85a257631bb8c29083ff312a1c0a6c827286590b5869c8d4`
and reversed SHA256
`8b62879eceff156a37298af0d8556a6eccad199cd566a9c6e34258ac18b62be2`.
Both native circuits lower through firtool. An attempted direct input-clock
association failed native dependency analysis and an executable Scala variant;
the complete fixture keeps the oracle's output-alias associations.

A fresh local invocation of the actual native compiler emits Rocket RTL and
collateral. Against the immutable U250 `design/FireSim-generated.sv` named
above, all 17 raw-input and 25 buffered-output clock assignments match. The
emitted clock-ready and completion predicates also match after wire expansion
and Boolean normalization, setting clock-valid to the bridge's constant valid
token as in iteration 22. Against the immutable
`design/FireSim-generated.implementation.xdc`, all three commands match clock
name, host-clock source, gate output, divide-by-one and setup/hold values.
The generated instance hierarchy spelling differs: the candidate retains
additional simulator wrappers, whose path is resolved by InstanceGraph.

Evidence is in mutable `iteration23-clock-boundary/`: `compiler-candidate/`,
`compiler.{stdout,stderr}.log`, native/Scala normal and reversed traces,
native MLIR/SV outputs, `rocket-domains.log`, `comparison.json`,
`build-final.log` and `tests-final.log`. All 11 focused CTest checks pass.
No harness-owned manager gates were launched in this iteration.

## Production clock bridge mapping (iteration 24)

`buildRationalClockChannel` now emits the complete ClockTokenVector producer:
constant valid, flipped ready and ordered Boolean vector lanes. The active
`addClockBridge` implementation uses it and consumes its returned lane values
for the fastest-clock counter.

The coupled fixture now invokes the production `addFAMEPipeWrapper`,
`addFAMEClockChannel`, `activateFAMEPipeWrapper` and `addClockBridge` functions.
Its interpreter follows actual instance arguments through the generated bridge,
pipe wrapper, target top and model; aggregate projection includes vector lanes
and flipped ready flow. The manual clock wire and direct scheduler construction
are gone. Original annotated IR is preserved as `.mapped.mlir`; only a verified
clone has raw annotations removed for backend lowering.

Production bridge preflight exposed inconsistent fixture MFMRs: periods 2 and
3 have MFMRs 1 and 2 relative to the fastest clock. Both native and Scala fixture
inputs now use these values throughout FAME and SimulationMapping. No Scala
compiler source or immutable oracle artifact was changed. Reversed order
changes physical clock identity, retaining the ordered rational ratios.

Fresh actual Scala FAME and rational-generator executions still match every
one of the 8192 native observations in each order, with the iteration 23 hashes
above. Both complete mapped native circuits lower through firtool. Six focused
CTest checks pass; after the final fixture metadata correction, both coupled
checks were rerun and pass.

A fresh invocation of the native Rocket compiler emits RTL and collateral.
Against the immutable primary
`firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json`,
the candidate clock name, ratio, MFMR and retained constructor match. Against
the immutable U250 `design/FireSim-generated.sv` named above, the mapped Rocket
producer's constant valid and lane plus all six decoded counter read words
match after interface and snapshot name normalization. The immutable fixtures
contain one clock; multiclock evidence comes from the actual Scala execution.

Evidence is in mutable `iteration24-bridge-mapping/`: `compiler-candidate/`,
compiler logs, `rocket-mapping.log`, producer MLIR/SV, mapped/lowering fixture
MLIR/SV, native/Scala traces, `trace-comparison.json`, `golden-comparison.json`,
build and test logs. No manager verification gates were launched.
Most large candidate MLIR snapshots are gzip-compressed; the five inference
and clock-mapping boundaries remain directly readable.

## Remaining work

The hub FAME boundary now uses all ordered clock domains. The active compiler
calls `addClockBridge`, which maps ordered rational lanes, their source/sink
targets and the fastest-clock counter. The actual Scala bridge comparison
is recorded in [RationalClockBridge.md](RationalClockBridge.md); the inner
scheduler comparison is in
[RationalClockTokenGenerator.md](RationalClockTokenGenerator.md). The coupled
payload fixture now also covers four production data queues; the differential
is recorded in [FAMEHubQueuedPipe.md](FAMEHubQueuedPipe.md). Next extend that
queued schedule to latency-one reset seeding and passive aggregate payloads.
The baseline supports one
model/clock hub and its bridge configuration remains Rocket specific.
Manager gates remain harness-owned; FAME-5 and the SFC UART-bearing differential
remain incomplete.
