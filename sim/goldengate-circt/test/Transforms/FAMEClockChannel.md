# Clock identities across simulator wrapping

`activateFAMEPipeWrapper` now distinguishes channel boundary endpoints from
`FAMEChannelConnectionAnnotation.clock`. Boundary endpoints move to simulator
ports; associated clocks keep their original target module and receive only
the new circuit prefix. The following ClockBridge mapping preserves those
identities through another circuit rename.

This distinction matters after `addFAMEClockChannel`: the wrapper exposes
Vec[Bool] tokens while the target retains Clock or ClockRecord payloads.
Previously a scalar `ticks.bits` clock became a vector aggregate, and a record
`ticks.bits._0` clock became an invalid vector selector. The new regression
failed on the previous production rewrite with `associated target clock moved
to Boolean interface`; it passes with the corrected rewrite.

The preserved Scala oracle is `SimWrapper.scala:332–337`: valid/ready pass
through and ClockRecord.elements zip with clock token lanes using asClock.
`SimulationMapping.scala` retains the inner modules and changes their circuit
name when linking; it filters FAME annotations afterward. CIRCT currently
retains FAME annotations for boundary inspection, so these clock references
must still identify the inner domains. This comparison does not claim that
final SFC retains the same annotations.

`FAMEClockChannelTest.cpp` checks scalar and three-lane payload truth tables,
each associated clock's exact original leaf identity, data endpoints moving to
wrapper ports, and preservation through actual `addClockBridge` producer
insertion. Eight malformed clock inputs still fail without IR mutation.
The captured Rocket mapping checks all 1,773 retained annotation classes,
ordered clock metadata, 42 associated Clock ports and 89 boundary endpoints.
It runs full MLIR verification after both wrapper stages.

The immutable artifacts compared are:

```text
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The annotation's ordered clock name, ratio 1:1 and MFMR 1 match the candidate.
The candidate wrapper's ready passthrough and ordered bit-zero connection,
combined with its lowered always-valid, constant-one producer, match the
recorded SFC SimWrapper/ClockBridgeModule clock interface. Rocket uses separate
clock alias ports, so it did not exhibit the record-selector defect; the new
scalar/record fixtures demonstrate it. No immutable multiclock fixture or
runtime failure is claimed.

Iteration 18 evidence is under the mutable generated directory
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration18-clock-identity/`:
`build.log`, `tests.log`, `rocket-mapping.log`, `rocket.wrapper.mlir`, extracted
`rocket.producer.mlir`/`.sv`, `lowering.log` and `comparison.json`.

Reproduce the native tests and captured mapping after sourcing the environment:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
gg_build="$gg_generated/goldengate-circt-build"
gg_evidence="$gg_generated/iteration18-clock-identity"
mkdir -p "$gg_evidence"
cmake --build "$gg_build" --target goldengate-circt goldengate-clock-channel-test goldengate-pipe-channel-test goldengate-ready-valid-channel-test -j4
ctest --test-dir "$gg_build" -R 'goldengate-(clock-channel|pipe-channel|ready-valid-channel)$' --output-on-failure
"$gg_build/goldengate-clock-channel-test" "$gg_generated/circt-ingestion/post-fame-clock-wrapper.mlir" "$gg_evidence/rocket.wrapper.mlir"
```

Next couple a two-clock producer and the transformed FAME hub with independent
data-channel stalls and compare target-state transitions to the executable
Scala oracle. Full multiclock simulator configuration, FAME-5 and the SFC
UART-bearing workload baseline remain pending. The supplied preceding
iteration's replacertl and Verilator regressions passed; the harness owns the
next manager gates for this change.
