# Rational ClockBridge token scheduling

`RationalClockTokenGenerator.cpp` ports the preserved inner generator in
`midas/src/main/scala/midas/widgets/ClockBridge.scala`. It creates FIRRTL
operations in an existing clock producer. `SingleClockBridge.cpp` now uses
this component on the active compiler path for every ordered constructor
clock. The wrapper, fastest-clock counter and annotation retargeting are
covered by [RationalClockBridge.md](RationalClockBridge.md).

The input is the ordered `clockInfo` array, a host Clock, synchronous UInt<1>
reset and downstream UInt<1> ready. `analyzeRationalClockSchedule` rejects an
empty array, nonpositive/out-of-Scala-Int ratios and countdowns wider than
16 bits before any IR mutation. Clock names and MFMR metadata are retained
by the caller; they do not alter scheduling. The emitter returns ordered
UInt<1> edge flags; valid is always one and is connected by the caller.

Normalized periods match `FindScaledPeriodGCD`'s arbitrary precision product
and GCD. The implementation instead reduces each period relative to lane 0,
uses the LCM of reduced denominators as normalized period 0, and scales each
relative numerator. This is the smallest integer period vector: dividing
its entries by any common factor would violate that minimal LCM. Positive
Int32 cross-products fit UInt64, and bounded division checks precede every
LCM/period multiplication. Large intermediate Scala products therefore do
not cause native overflow. Period 65535 is accepted; 65536 is rejected.

For multiple lanes, all host-clocked countdowns synchronously reset to zero.
A balanced minimum reduction feeds each lane's equality comparison. When
ready is high, matching lanes reload their normalized period and other
lanes subtract the minimum. Otherwise every countdown holds. Register
updates use old state simultaneously. This skips empty target timesteps,
always emits at least one edge, and holds the complete token during stalls.
One lane emits a constant one, matching the optimized SFC golden RTL.

## Executable oracle and comparison

`RationalClockOracle.scala` instantiates the preserved actual
`ClockBridgeModule.RationalClockTokenGenerator`, emits Chisel FIRRTL, and
runs SFC `LowFirrtlCompiler`. Its interpreter evaluates the emitted nodes,
connects and register updates; it does not implement a second scheduler.
`RationalClockTokenGeneratorTest.cpp` evaluates the native emitted SSA
operations with the same deterministic ready/reset stimuli.

Nine cases cover one lane, base/half rate, periods 2/3 in either lane order,
three periods 2/3/4, unreduced ratios, equal ratios including Int32 maximum,
large cross-products, and the 16-bit limit. The limit case runs 200,000 host
cycles; others run 4,096. Backpressure and midstream reset are included.
All 232,768 ordered token/valid observations and nine period/width records
match exactly. Native tests additionally reject eight malformed/oversized
schedules and normalize multiple coprime large equal ratios to all ones.
All nine candidate IR fixtures verify and lower through CIRCT to RTL.

Iteration 16 evidence is in the mutable generated directory:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration16-rational-clock/
```

It contains `build.log`, `tests.log`, `scala-observations.log`,
`circt-observations.log`, empty `sequence.diff`, `comparison.json`, and the
nine SFC FIRRTL / native MLIR / native SystemVerilog fixtures. Three focused
CTest checks pass: rational-clock-token, single-clock-bridge (20,000 cycles
including counter overflow and snapshot semantics), and clock-bridge-header.
The actual `goldengate-circt` executable is rebuilt with the component.

The exact immutable baseline artifacts compared are:

```text
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The primary annotation contains one constructor/channel clock with matching
name, ratio 1:1 and MFMR 1. The corresponding U250 `ClockBridgeModule` assigns
`hPort_clocks_valid` and `hPort_clocks_bits_0` to one. Candidate `single.circt.sv`
assigns `valid` and `bit0` to one: the scheduling boundary matches. The other
cases use freshly executed Scala references because the recorded immutable
Rocket fixture has one clock.

To reproduce the native observations, first source the required environment:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
gg_build="$gg_generated/goldengate-circt-build"
gg_evidence="$gg_generated/iteration16-rational-clock"
mkdir -p "$gg_evidence/classes"
cmake --build "$gg_build" --target goldengate-circt goldengate-rational-clock-token-test goldengate-single-clock-bridge-test goldengate-clock-bridge-header-test -j4
ctest --test-dir "$gg_build" -R 'goldengate-(rational-clock-token|single-clock-bridge|clock-bridge-header)$' --output-on-failure
"$gg_build/goldengate-rational-clock-token-test" --observations "$gg_evidence" > "$gg_evidence/circt-observations.log"
gg_classpath="sim/midas/target/scala-2.13/classes:$(cat sim/midas/target/streams/compile/dependencyClasspath/_global/streams/export)"
gg_compiler=/home/firesim/.cache/coursier/v1/https/repo1.maven.org/maven2/org/scala-lang/scala-compiler/2.13.10/scala-compiler-2.13.10.jar
java -cp "$gg_classpath:$gg_compiler" scala.tools.nsc.Main -classpath "$gg_classpath" -d "$gg_evidence/classes" sim/goldengate-circt/test/Transforms/RationalClockOracle.scala
java -cp "$gg_evidence/classes:$gg_classpath" RationalClockOracle "$gg_evidence" > "$gg_evidence/scala-observations.log"
diff -u "$gg_evidence/scala-observations.log" "$gg_evidence/circt-observations.log"
```

## Remaining boundary

The ordered Vec[N] wrapper now calls this scheduler for all retained clocks,
validates MFMR against ceil(period/minimumPeriod), and preserves Scala's
fastest-clock counter and six-word driver ABI. See the full bridge comparison
linked above. A coupled multiclock ClockBridge-to-FAME hub differential,
Rocket-specific bridge assumptions, FAME-5 and the missing SFC UART-bearing
workload reference remain unresolved. FireSim manager verification remains
harness-owned.
