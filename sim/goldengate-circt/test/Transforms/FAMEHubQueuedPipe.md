# Queued data channels coupled to the FAME clock hub

`FAMEHubClockCoupledTest --queued` retains post-FAME data boundary annotations
and calls the production `addFAMEBoundaryPipeChannels`, `addFAMEPipeWrapper`,
clock channel and ClockBridge builders. Four UInt16 latency-zero queues share
one definition and retain separate instance state. Two input and two output
channels use rational domains 1/2 and 1/3, with independent valid/ready stalls,
initial host reset and another reset at host cycles 4096–4097. The existing
direct-channel modes remain available.

`addFAMEPipeWrapper` now validates the complete queue interface before creating
any wrapper operations. A matching symbol must also provide all eight ports
in the expected order with their exact names, directions and payload types.
`FAMEPipeChannelTest` checks atomic rejection of missing, reordered, reversed,
wrong-width and equal-width signed ports. This protects the positional typed
connects used by `SimWrapper.genPipeChannel`.

The native interpreter privately copies repeatedly instantiated definitions
for evaluation only; exported compiler IR retains one queue definition and
four instances. The Scala oracle uses the preserved `FAMETransform`, actual
`ClockBridgeModule` rational producer and four Chisel `midas.core.PipeChannel`
instances. Its hierarchical LowFIRRTL interpreter keys registers by instance
path. Independent FIFO scoreboards check capacity, valid and front payloads.
Every queue reaches depth two in both clock orders.

## Differential evidence, iteration 26

Artifacts are under the generated Rocket directory's `iteration26-queued-pipe`.
Each trace covers 8192 host cycles and contains the original hub fields plus
external input-ready/output-valid/output-payload fields and queued hub inputs.
Invalid external output payloads are canonicalized to zero; target state and
hub payloads are compared on every cycle.

| Ordered clock payload | Matching TRACE SHA256 |
| --- | --- |
| Normal | `60cd1edaa7bc42d140a3cbfcd6873c7fb3f8798b7582f2a7bc6a401f53930119` |
| Reversed | `4ffa6e8164bd4e60e774a2225911cdeb64eb81a57a948c59457df7dcbe50f34b` |

Both orders match all 19 numeric trace fields with zero mismatches. Normal
queue deliveries (input0,input1,output0,output1) are 1133,755,1131,755;
reversed deliveries are 655,982,656,980. Both target accumulator trajectories
match, clocks remain disabled through host reset, and blocked output payloads
remain stable. This compares emitted FIRRTL graph behavior, not a copied hub
recurrence.

Reproduce after sourcing `sourceme-manager.sh --skip-ssh-setup`:

```sh
$gg_build/goldengate-fame-hub-clock-coupled-test --queued "$gg_e/queued.mlir" > "$gg_e/native.trace"
$gg_build/goldengate-fame-hub-clock-coupled-test --queued-reversed "$gg_e/queued-reversed.mlir" > "$gg_e/native-reversed.trace"
# Compile the oracle with the existing midas dependency classpath and the
# Scala 2.13.10 / Chisel 3.6.1 compiler plugin, then:
java -cp "$gg_e/scala-classes:$gg_classpath" FAMEHubQueuedPipeOracle "$gg_e/scala" > "$gg_e/scala.trace"
java -cp "$gg_e/scala-classes:$gg_classpath" FAMEHubQueuedPipeOracle "$gg_e/scala-reversed" reversed > "$gg_e/scala-reversed.trace"
```

Compare only lines beginning `TRACE `; the Scala run also emits a FIFO coverage
summary. CTest includes both queued orders and both existing direct orders.

## Recorded SFC boundary comparison

The immutable U250 artifact compared is:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Fresh Rocket `compiler-candidate/post-fame-first-pipe-wrapper.mlir` matches all
eight port names, directions and widths for golden `PipeChannel`,
`PipeChannel_1`, `PipeChannel_2` and `PipeChannel_7` (UInt32 latency one, UInt1
latency zero, UInt1 latency one and UInt40 latency one). The golden zero-width
`PipeChannel_6` and `PipeChannel_10`, including their underlying ShiftQueues,
match freshly lowered `native/zero-1.sv` across all 128 combinations of valid
state, initialization, reset and handshake inputs; their six physical ports
also match. Reports are `golden-interface-comparison.json` and
`golden-zero-comparison.json` in the evidence directory.

The full recorded/current Rocket queue sets still differ: the recorded RTL
has zero-width trace payloads, while current ingestion also emits UInt3/UInt64
queues. That preexisting boundary difference is retained explicitly and this
change does not claim full Rocket RTL equivalence. Both queued fixtures lower
to SystemVerilog, and all five focused CTests pass. A fresh native Rocket
`--compile-baseline` also completes through RTL and collateral emission.

## Limits and next step

This fixture uses one model/hub, two rational domains, one ClockBridge, scalar
UInt16 payloads and latency-zero data queues. Extend the same differential to
latency-one reset tokens before broadening aggregate payload coverage. The
Rocket build configuration, FAME-5 support and pending SFC UART-bearing
workload baseline remain separate gaps. FireSim manager verification remains
owned by the harness.
