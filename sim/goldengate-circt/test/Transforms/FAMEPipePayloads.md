# Typed PipeChannel payloads

`FAMEPipeChannel.cpp` now constructs depth-two queues with the original
FIRRTL payload type: UInt (including width zero), SInt, passive bundles, and
vectors of integer leaves with known widths. One aggregate payload transfers
as one token; all leaves share occupancy, backpressure and initialization.
Latency one inserts `0.U.asTypeOf(gen)` after reset, using a FIRRTL bitcast of
a packed zero. Latency zero starts empty. Payload registers remain unreset,
while the two occupancy registers reset to false, matching the preserved
`midas/core/Channel.scala` and Rocket Chip `ShiftQueue`.

Module sharing uses payload type and latency. Legacy UInt names are preserved;
SInt names include signedness and aggregate names include a deterministic
SHA256 of the full FIRRTL type. Equal packed widths with different signedness,
field names or shapes produce separate definitions. Unknown widths, flipped
payload fields, const types, clocks/resets and analog leaves fail before IR
mutation. Multiple annotation endpoints, loopbacks and fanout remain unsupported.

The standalone typed builder and the production boundary/wrapper path both
use this implementation. `FAMEPipeChannelTest.cpp` verifies typed wrapper
connections, endpoint retargeting, definition separation, atomic rejection,
and 200,000 randomized queue cycles against a FIFO reference. Its optional
output-directory argument emits isolated native MLIR queues and 4,096 cycle
observations for four payloads at latencies zero and one.

## Iteration 25 evidence

Mutable evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration25-pipe-payload/`.

The immutable U250 golden artifact compared is:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Its `PipeChannel_6` (line 25823) and `PipeChannel_10` (line 26190) carry
zero-width payloads and instantiate `ShiftQueue_36` and `ShiftQueue_40`.
Their six handshake/control ports match native `GGFAMEPipe0` after lowering;
both omit payload ports. Evaluating the actual emitted Boolean expressions
and register assignments matches both golden modules for all 64 combinations
of occupancy, initialization, reset, input-valid and output-ready: 128 checks,
zero mismatches. `golden-zero-comparison.json` records the result and exact
artifact paths; `compare-golden-zero.py` is the local comparison artifact.
The observed width-zero gap selected this compiler change.

`FAMEPipePayloadOracle.scala` elaborates and interprets the preserved actual
Chisel `PipeChannel` / `ShiftQueue` hierarchy for UInt0, SInt13, UInt5[3], and a
nested 32-bit bundle containing signed/unsigned fields and a vector. Native
and Scala traces match all 4,096 rows under stalls, reset bursts and signed
bit patterns; SHA256 of the filtered trace is
`670da59015a7f8df38ba09209ff08048cdab60d626b2862ece89aa9558821cfb`.
All eight native and eight Scala fixtures lower to RTL using the project
`firtool-1.75.0`. Signed/aggregate evidence supplements the immutable fixture,
which contains only scalar UInt PipeChannels. The Scala `SimWrapper` type
reconstruction has no vector case; the vector oracle directly exercises the
production generic `PipeChannel`, not that reconstruction path.

Six focused CTests pass: PipeChannel, ReadyValidChannel, ClockChannel,
SingleClockBridge, and normal/reversed coupled clock-hub fixtures. A fresh
native Rocket compiler invocation completed and emitted simulator RTL in
`compiler-candidate/`. Its six PipeChannel definitions retain the previous
iteration's port/latency contracts: UInt1 (both latencies), UInt3, UInt32,
UInt40 and UInt64. Four definitions match the immutable golden
width/latency/port classes; the UInt3/UInt64 classes have no matching golden
definition, and the Rocket boundary still omits the golden zero-width pipes.
These differences predate this change and are recorded in
`rocket-pipe-comparison.json`; the new zero-width construction is verified by
the focused golden comparison, not by claiming full Rocket boundary parity.
Manager compile/metasim/bitstream gates remain owned by the verification
harness; previous iteration's Rocket suite passes do not validate this change.

## Reproduce the executable Scala oracle

From `sims/firesim`, source `sourceme-manager.sh --skip-ssh-setup` first. Build
`goldengate-circt` and `goldengate-pipe-channel-test` in the configured native
build. Run the test with an existing output directory to emit native fixtures.
For Scala, use the existing Midas classes and dependency classpath:

```bash
pipe_classpath="sim/midas/target/scala-2.13/classes:$(cat sim/midas/target/streams/compile/dependencyClasspath/_global/streams/export)"
pipe_cache=/home/firesim/.cache/coursier/v1/https/repo1.maven.org/maven2
pipe_compiler=$pipe_cache/org/scala-lang/scala-compiler/2.13.10/scala-compiler-2.13.10.jar
pipe_plugin=$pipe_cache/edu/berkeley/cs/chisel3-plugin_2.13.10/3.6.1/chisel3-plugin_2.13.10-3.6.1.jar
java -cp "$pipe_classpath:$pipe_compiler" scala.tools.nsc.Main \
  -classpath "$pipe_classpath" -Xplugin:"$pipe_plugin" \
  -d "$pipe_evidence/scala-classes" \
  sim/goldengate-circt/test/Transforms/FAMEPipePayloadOracle.scala
java -cp "$pipe_evidence/scala-classes:$pipe_classpath" \
  FAMEPipePayloadOracle "$pipe_evidence/scala" > "$pipe_evidence/scala.trace"
```

Create the evidence/classes directories beforehand. Compare only `TRACE`
rows, since Chisel emits elaboration messages as well. The oracle models
handshake/state equations from emitted low FIRRTL, rather than copying queue
recurrences. Interpret invalid output payloads as zero for comparison.

The next integration step is to place the actual data queues into the coupled
rational-producer/FAME-hub fixture and compare against the Scala mapping,
including multiply instantiated queue definitions and independent stalls.
