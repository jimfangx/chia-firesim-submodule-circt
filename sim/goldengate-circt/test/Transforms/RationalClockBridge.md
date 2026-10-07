# Ordered rational ClockBridge mapping

`SingleClockBridge.cpp` now implements `addClockBridge` on the actual
`goldengate-circt` compiler path. It maps the retained ordered ClockParameters
constructor into a Vec[N] FIRRTL clock producer. The `GGSingleClockBridge`
module symbol and six-word 32-bit decoded MCR interface remain compatible with
the existing control-bus and driver-header consumers.

Preflight requires constructor clocks to equal channel clockInfo, one ordered
bits[i] sink on the same active wrapper port per clock, positive Scala Int
ratios, and matching per-clock MFMR. MFMR is ceil(normalized period / minimum
period); countdown width remains at most 16 bits. Duplicate, reordered or
missing sinks and malformed schedules fail before mutating IR. Names, clock
metadata and lane order survive source/sink retargeting. The host-side bridge
accepts rational schedules without a 1:1 lane, matching ClockBridgeModule;
the target-side RationalClockBridge constructor's base-clock requirement is a
separate input contract.

The emitter uses `buildRationalClockChannel` for the complete Decoupled vector,
including `buildRationalClockTokens` for every lane. Valid remains one;
ready advances countdowns and stalls hold the complete vector. hCycle advances
each host cycle. tCycleFastest advances only when an accepted token contains
the selected fastest lane. Selection follows the Scala Double frequency sort,
including the last lane on a tie. Synchronous reset clears both counters and
countdowns. Unreset latch registers capture pre-edge counters on write-data
bit 0; the read bank exposes saved low/high halves at words 0/1 and 3/4 and
zero at write-only words 2/5. Offsets, permissions and driver constructor
collateral are independent of the number of clocks.

## Executable comparison

`ClockBridgeOracle.scala` elaborates the preserved actual LazyModule
ClockBridgeModule, runs SFC LowFirrtlCompiler and evaluates its emitted top
and actual countdown child together. The oracle injects writes at the decoded
MCR boundary; Nasti transport is outside this comparison. Fastest-lane selection
is extracted from the actual target-counter expression, and register allocation
comes from the actual bridge's crRegistry. No production Scala source changes.

`SingleClockBridgeTest.cpp` evaluates the candidate FIRRTL operations with
identical ready/reset/latch stimuli. Nine schedules cover one lane, 1:1/1:2,
periods 2/3 in both orders, three periods 2/3/4, unreduced ratios, equal ratios
including Int32 maximum, large cross-products and period 65535. Eight run 4096
host cycles; the limit runs 200000. Reset occurs initially and midstream;
backpressure and writes with data bit 0 clear/set exercise both counters and
pre-edge snapshots. All 232768 BRIDGE observations, nine FASTEST selections
and nine REGISTERS maps match exactly. Native tests additionally retain 20000
single-lane counter/latch/overflow observations and check 44 atomic rejections.
The header test confirms identical constructor collateral for one, two and
three lanes. Three focused CTest checks pass, and all nine standalone native
producers lower through CIRCT to SystemVerilog.

The actual captured Rocket `circt-ingestion/post-fame-first-pipe-active-wrapper.mlir`
also passes this mapping and full MLIR verification; its extracted producer
lowers to RTL. The immutable artifacts compared are:

```text
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The primary annotation's single ordered clock name, ratio 1:1 and MFMR 1 match
the candidate. Both retargeted endpoints resolve. The U250 ClockBridgeModule's
always-valid single bit and six decoded read expressions match the candidate
after normalizing interface and snapshot register names. The golden has one
clock; multiclock evidence comes from the freshly executed actual Scala bridge,
not an immutable multiclock reference.

Iteration 17 evidence is in the mutable generated directory:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration17-clock-bridge/
```

It contains `final-build.log`, `tests.log`, Scala/native observations, empty
`sequence.diff`, `comparison.json`, nine full SFC FIRRTL references and native
bridge/producer MLIR/RTL fixtures, plus `rocket-mapping.log`, `rocket.wrapper.mlir`
and `rocket.producer.mlir`/`.sv`. The build includes the actual compiler driver.

To reproduce, source the project environment first:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
gg_build="$gg_generated/goldengate-circt-build"
gg_evidence="$gg_generated/iteration17-clock-bridge"
mkdir -p "$gg_evidence/classes"
cmake --build "$gg_build" --target goldengate-circt goldengate-single-clock-bridge-test goldengate-clock-bridge-header-test goldengate-rational-clock-token-test -j4
ctest --test-dir "$gg_build" -R 'goldengate-(rational-clock-token|single-clock-bridge|clock-bridge-header)$' --output-on-failure
"$gg_build/goldengate-single-clock-bridge-test" "$gg_evidence" > "$gg_evidence/circt-observations.log"
gg_classpath="sim/midas/target/scala-2.13/classes:$(cat sim/midas/target/streams/compile/dependencyClasspath/_global/streams/export)"
gg_compiler=/home/firesim/.cache/coursier/v1/https/repo1.maven.org/maven2/org/scala-lang/scala-compiler/2.13.10/scala-compiler-2.13.10.jar
java -cp "$gg_classpath:$gg_compiler" scala.tools.nsc.Main -classpath "$gg_classpath" -d "$gg_evidence/classes" sim/goldengate-circt/test/Transforms/ClockBridgeOracle.scala
java -cp "$gg_evidence/classes:$gg_classpath" ClockBridgeOracle "$gg_evidence" > "$gg_evidence/scala-observations.log"
"$gg_build/goldengate-single-clock-bridge-test" --map-handoff "$gg_generated/circt-ingestion/post-fame-first-pipe-active-wrapper.mlir" "$gg_evidence" > "$gg_evidence/rocket-mapping.log"
firtool "$gg_evidence/rocket.producer.mlir" --format=mlir --verilog -o "$gg_evidence/rocket.producer.sv"
```

Filter FASTEST, REGISTERS and BRIDGE lines for the structured differential;
native output also includes a final success message. No FireSim manager gate
was started in this iteration. The supplied preceding iteration's replacertl
and Verilator workloads passed, but those results do not validate this change.

## Remaining boundary

Next connect a two-clock producer and the transformed FAME hub in one
differential fixture with independent data-channel stalls, checking buffered
enable/target state transitions as well as accepted clock tokens. Full
multiclock simulator configuration, Rocket-specific bridge assumptions, FAME-5,
memory threading and the SFC UART-bearing workload baseline remain pending.
