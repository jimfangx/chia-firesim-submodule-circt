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

## Remaining work

The baseline driver still requires one hub clock because downstream data
channels use one enable. Next, use the ordered records for all clock annotation
renames and input/output FSM clock-enable selection before enabling multiple
clocks in the full hub path. Manager gates remain harness-owned;
FAME-5 and the SFC UART-bearing differential remain incomplete.
