# Archived U250 compiler replay, iteration 167

The native FAME input normalization now runs CIRCT DropConst after ground
type lowering and before ExpandWhens. SFC's LowForm input to FAME has no
constant type qualifier. CIRCT's imported constant predicates caused
ExpandWhens to construct an ordinary UInt conjunction with a const UInt
result in BOOM, Gemmini and Shuttle. The verifier rejected those operations.
Dropping the qualifier preserves the literal values and retained targets.

The regression in `Analysis/CombDependencyAnalysisTest.cpp` reproduces that
failure with a constant inner predicate under a dynamic outer `when`. It
checks verified normalized IR, retained output identity, dependencies and
the output selection for 32 input combinations. Both `goldengate-comb-dependency`
and `goldengate-lower-types-targets` CTests passed. The fixture failed with
the same const/UInt verifier diagnostic before adding DropConst.

`sim/make/goldengate.mk` now passes the actual `PLATFORM_CONFIG` to the native
compiler. Base U250 and the existing AutoILA fragments are supported;
unported partition/QSFP profiles fail before output creation. This exposes
an implementation gap instead of applying base U250 semantics to a partition.
All thirteen recipe profiles were checked through Make dry runs. Six invalid
CLI cases checked missing/empty/duplicate configurations and invalid ILA depth.

## Replay and comparison

The immutable matrix manifest is:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/manifest.json
```

`U250TargetMatrix.py` invokes the actual native compiler using each recipe's
archived `.sfc.fir`, `.anno.json` and effective platform configuration, with
separate candidate directories. It compares full annotation record multisets,
including constructors, targets, ordered endpoints and clocks. A failed,
unsupported or mismatched row makes the matrix return nonzero.

Source `sourceme-manager.sh --skip-ssh-setup` before running the standalone test:

```bash
python3 sim/goldengate-circt/test/U250TargetMatrix.py \
  --manifest /scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/manifest.json \
  --compiler <native-build>/goldengate-circt --output-dir <fresh-output> --jobs 2
```

This run's artifacts are under:

```
/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration167-u250-target-matrix/
```

`replay/matrix.json` and `replay/matrix.md` retain all thirteen rows and exact
commands, hashes, logs, candidates and comparison paths. Native compiler SHA256:
`67e051326d16d3475dbb667180b1e66e41618b6e54070094567e830d2a5eab80`.
`initial-inventory.json` records the pre-fix base-target failures. Comparisons
were extended to model-port annotations after replay, without recompiling.

| Target | Native result after normalization fix | Bridge/channel records |
| --- | --- | --- |
| Rocket single | Compiled | 8 / 43 matched |
| Rocket quad | Multiple TracerV constructors unsupported | 11 / 79 matched |
| BOOM | Wider TracerV constructor unsupported | 8 / 62 matched |
| Rocket with NIC | Optional widget constructor key rejected | Not reached |
| Gemmini Rocket | FASED maxFlight=20 unsupported | 8 / 43 matched |
| Saturn Rocket | FASED maxFlight=20 unsupported | 8 / 43 matched |
| Saturn Shuttle | Wider TracerV constructor unsupported | 8 / 51 matched |
| Dual Rocket split base, 0, 1 | Three distinct partition/QSFP rejections | Not reached |
| Quad Rocket ring base, 0, 1 | Three distinct partition/QSFP rejections | Not reached |

For each reached bridge boundary, the exact SFC artifact is
`<manifest target golden_dir>/post-bridge-extraction.json`, compared with
`<candidate_dir>/post-bridge-extraction-all.json`.

Shuttle's SFC artifact
`alveo_u250_firesim_saturn_GENV256D128_shuttle_singlecore_no_nic/design/post-infer-model-ports.json`
in the archive matches the candidate's `post-infer-model-ports.json` exactly
for 51 channel connections, 51 model channel ports, seven fanout records,
one host clock, one host reset and one model transform record. Five other
reached targets have additional native SRAM model/channel records; those
model-port comparisons remain mismatches requiring separate equivalence
checks. They are not waived based on a bridge-boundary match.

The archived Rocket input FIRRTL and annotations are SHA256-identical to
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json` in the
recorded immutable `firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01`
compiler fixture. The emitted Rocket `FireSim-generated.synthesis.xdc` also
matches nonempty lines in the immutable U250 build-tree fixture's `design/`
directory. `FireSim-generated.implementation.xdc` differs at the buffer path:
SFC uses `target/FireSim_`, while the candidate uses `target_FAMETop/FireSim`.
Clock name, divide ratio and multicycle constraints agree; this is a hierarchy
rename, not textual equality. Details are in `immutable-rocket-comparison.json`.

This is one compiled target and twelve compile gaps, not overall parity.
No manager verification or new-target metasimulation was started here.
The next small ingestion fix is accepting an absent SimpleNIC constructor
key as Scala's valid `Option.None`; NIC token-engine behavior must then be
ported. Wider/multiple TracerV, parameterized FASED geometry and partition/QSFP
semantics remain core work, with behavioral and platform/ABI validation needed.

## Optional bridge constructors, iteration 168

`BridgeAnalysis.cpp` now follows `BridgeAnnotation.toIOAnnotation`: copy the
optional serializable constructor value unchanged, and omit the field when
the source annotation has no constructor. The NIC stub's `constructorArg = None`
is valid; promotion previously rejected it because it required a dictionary.
Constructor interpretation remains the responsibility of each widget lowering.

The new `goldengate-bridge-promotion` CTest passed. It exercises missing,
dictionary and string constructor values on actual CIRCT bridge instances,
checks verified IR after instance removal, and checks the preserved data/clock
connections, endpoint targets, channel mapping and unrelated annotations.

Native replay artifacts for this compiler snapshot are under:

```
/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration168-u250-target-matrix/
```

Native compiler SHA256:
`d9dacf2ff8c50db19a68e0c0f0ba8013b5ef44da34de2172ccd0c807ef19b01e`.

The exact NIC SFC golden files in the immutable matrix archive are:

```
alveo_u250_firesim_rocket_singlecore_nic/design/post-bridge-extraction.json
alveo_u250_firesim_rocket_singlecore_nic/design/post-bridge-extraction.fir
alveo_u250_firesim_rocket_singlecore_nic/design/post-wrap-top-all.json
alveo_u250_firesim_rocket_singlecore_nic/design/post-infer-model-ports.json
```

The candidate's `post-bridge-extraction-all.json` matches all nine complete
BridgeIO records and 58 channel connection records. Both sides omit the NIC
constructor key. The FIRRTL top-port comparison agrees for all nine bridge
ports, including all sixteen NIC leaves: field names, types/widths and effective
directions match after normalizing whitespace, port order and source locations.
`nic-bridge-port-comparison.json` retains the structured port contracts.

The candidate's `post-wrap-top-all.json` also matches the nine BridgeIO and
58 channel connection records, including bridge clock metadata and wrapped
targets. `nic-wrap-clock-comparison.json` retains that comparison. The model-port
boundary remains a mismatch: SFC has 58 connections, 58 model channel ports
and one transformed model; CIRCT has 68, 78 and two due to its additional SRAM
model. Eighteen fanout records, host clock and host reset agree. This requires
separate SRAM equivalence work rather than being treated as parity.

NIC replay now reaches `post-fame-cpu-stream-platform.mlir` and then fails with
`FPGA top shell: FPGATop shell would hide an input port`. Its complete stdout
and stderr are retained in the NIC replay directory. No NIC RTL or driver
support is claimed. The next small porting step is materializing SimpleNIC's
host token queues and connecting its fifteen pipe channels; the seven-token
512-bit CPU stream adapters and MMIO/driver ABI must follow. Keep the shell's
input-port check while those channels remain unbound.

The complete thirteen-target native replay finished with one compiled target,
six lowering failures and six unsupported partition profiles. All seven base
targets match their bridge/channel annotation boundaries. Shuttle also matches
the model-port boundary; the other six base targets retain SRAM-model
differences. The replay correctly exits nonzero. Its full per-target logs,
structured comparisons and actual effective profiles are in `replay/matrix.json`
and `replay/matrix.md`; no new manager gates were run.

The recorded 2026-10-01 immutable Rocket fixtures were also checked against this
snapshot. Archived pre-MIDAS FIRRTL and annotation SHA256 values still match the
immutable compiler inputs. Candidate `FireSim-generated.synthesis.xdc` agrees
with the immutable U250 design's nonempty constraint lines. Implementation XDC
agrees after the previously recorded `target/FireSim_` to
`target_FAMETop/FireSim` hierarchy rename, retaining the same clock name, divide
ratio and multicycle constraints. Exact fixture/candidate paths and results are
in `immutable-rocket-comparison.json`; this does not establish RTL equivalence.


## Iteration 169: native SimpleNIC token queues and channel gates

`addNICTokenQueues` is now part of the active native baseline compiler after
CPU stream platform assembly. A constructor-free SimpleNIC widget binds its
fifteen latency-one channels to two instantiated depth-ten FIRRTL queues.
No NIC annotation leaves the circuit unchanged. A malformed or unsupported
NIC boundary is rejected before any mutation. This is a partial NIC lowering:
seven-token CPU stream packet adapters, stream allocation, MMIO registers and
the driver ABI remain unimplemented. Compilation emits the verified
`post-fame-nic-token-queues.mlir` and annotation boundary, then reports these
missing bindings; it does not emit an incomplete FPGA design as a success.

Both queues store the 66 nonconstant packet bits (64 data, last, valid), with
asynchronous reads, synchronous writes, host-reset pointer/occupancy state,
non-power-of-two pointer wrap at nine, and flow/pipe disabled. Incoming keep
is FF. The later outgoing packet adapter must restore the constant
`data_in_ready = true` field. Target reset does not flush these queues.
All four outgoing channels participate in the token handshake, including
keep, whose data the host packet adapter discards. Each ready/valid gate
excludes only its own channel predicate. Host-to-target and target-to-host
queue progress are independent. Seven configuration inputs remain explicit;
`nic_done` exposes the combinational source for the later sampled MMIO register.

The new `goldengate-nic-token-queues` CTest passes on actual emitted operations:
12,000 randomized channel-gate cases, all fifteen isolated channel stalls,
queue-capacity stalls, independent direction progress, queue geometry and
instantiation, consumed endpoint retention, copied clock transfer, and
pre-mutation rejection of latency-two, duplicate and inconsistent clock
annotations. A regression fixture also accepts and preserves the historical
FAMETop clock identity after its physical port
has been removed by FAME lowering.

`test/Transforms/NICTokenQueuesCompare.py` compares the emitted native queue
RTL to these exact immutable SFC modules:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
Queue_17 (host-to-NIC), Queue_18 (NIC-to-host)
```

A standalone Verilator unit simulation matches every ready/valid value and
valid 66-bit payload against both SFC queues and an independent occupancy and
ordering scoreboard over 40,000 cycles: 363 full, 2,561 empty, 20,809 simultaneous
enqueue/dequeue, 353 reset, 29,610 enqueue and 28,032 dequeue cycles. Comparison
of the SFC FPGA wrapper's fifteen exclusion gates also matches the predicates
exercised by the CTest. This is unit RTL simulation, not a FireSim metasim gate.

Evidence is retained under the fresh repository output directory:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration169-u250-target-matrix/
```

`nic-queues-final/comparison.json`, `sfc-gate-contracts.json`, generated native/SFC
queue RTL and complete unit build/behavior logs retain this comparison.
The next smallest porting step is the seven-token 512-bit packet adapters,
including stalled seventh-token handling and their host-reset counters, before
allocating NIC CPU streams and binding control/driver collateral.

The actual archived NIC input also reaches this boundary in `replay-final`.
All 1,792 annotation records survive queue assembly, with only the explicit
circuit and copied-port target transfer; consumed channel endpoints still
name the retained inner module. `nic-annotation-transfer.json` records the
exact comparison. Its nine BridgeIO and 58 channel connection records at
bridge extraction still match the SFC `post-bridge-extraction.json` boundary.
The later model-port boundary retains the previously recorded SRAM mismatch
(68 native channel connections versus 58 SFC); queue assembly does not claim
to resolve it. The NIC row remains a compile failure with the specific
packet-adapter/MMIO diagnostic despite this verified partial lowering.

The first replay in `replay` diagnosed an overly strict attempt to resolve
the historical `~GGCPUStreamPlatformWrapper|FAMETop>ep_3_clock` as a present
port. SFC `FAMETransform.staleTopPort` removes target Clock ports while
retaining this earlier channel clock identity. The corrected implementation
requires a common retained clock string across the fifteen channels and
preserves that identity; it uses the explicit host clock for queue state.
`replay-final` contains the corrected compiler replay, and the initial logs
remain available for that diagnosis.

The final Rocket candidate RTL is unchanged from iteration 168. The recorded
2026-10-01 immutable compiler input hashes and normalized U250 XDC constraints
still agree, including the documented implementation hierarchy rename.
`no-nic-rtl-comparison.json` and `immutable-rocket-comparison.json` record the
exact final candidate and golden paths. No manager verification gate was run.

The corrected full thirteen-target replay completed with compiler SHA256
`62bccecc3f0780ee4d5209040a2ecc24c68956a308379c1bc44a8a2c9f65f9b1`.
`replay-final/matrix.json` and `matrix.md` retain one compiled target, six
lowering failures and six unsupported partition profiles, using every
recipe's exact archived inputs and effective platform configuration. All
seven base bridge/channel boundaries match; Shuttle's model-port boundary
also matches, while the other six retain SRAM differences. The complete
replay exits nonzero. Remaining failures are multiple/non-Rocket TracerV,
FASED outstanding profiles beyond maxFlight=10, NIC packet/MMIO bindings,
and partition/QSFP semantics. Compilation and annotation matches are partial
evidence, not target behavioral or platform/ABI parity.


## Iteration 170: native SimpleNIC packet adapters

`addNICPacketAdapters` now runs after native NIC token queue assembly in the
baseline compiler. It instantiates a decoder and encoder between the existing
66-bit queue ports and explicit 512-bit CPU stream ports. The decoder holds
its 32-bit token index on stalls and accepts a CPU packet only when the
seventh NIC token is accepted. The encoder buffers six tokens in unreset
registers, bypasses the seventh directly, and holds its index during output
backpressure. Its 43-bit packet trace counter increments only on an accepted
512-bit output packet. Host reset resets the three counters, preserving
buffer contents. Payload layout is data `[511:64]`, seven last/ready/valid
triplets `[63:43]`, and the packet counter `[42:0]`. Outgoing ready bits are
one; incoming ready bits are unused at the queue boundary, matching the
normal SimpleNIC widget. Loopback and multiple NIC modes remain unsupported.

The compiler verifies and emits `post-fame-nic-packet-adapters.mlir` and
`post-fame-nic-packet-adapters-all.json`. Stream allocation and MMIO control
binding still cause an explicit compilation failure. The new boundary does
not claim that a complete NIC FPGA design or driver is supported.

`goldengate-nic-packet-adapters` CTest passes: verified FIRRTL geometry and
instantiation, six unreset 66-bit buffers, 32/43-bit reset counters, consumed
and copied target transfers, wrong-width rejection before mutation, repeated
pass rejection, and no-NIC preservation. The standalone RTL fixture connects
both instantiated adapters through a complete test top.

`test/Transforms/NICPacketAdaptersCompare.py` lowers that actual native IR
with firtool and compares it to the exact SFC modules
`BigTokenToNICTokenAdapter` and `NICTokenToBigTokenAdapter` in:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Ready/valid and payload comparisons pass over 40,000 standalone Verilator
unit cycles, supplemented by independent packet-format and transaction
scoreboards. Coverage: 21,318 decoded and 30,039 encoded token transactions,
10,611 decoder stalls, 2,036 stalled seventh output tokens, 4,134 bypassed
packets, 353 resets, and 14,345 input bubble cycles. This is compiler-unit RTL
simulation; FireSim manager verification remains owned by the harness.

Evidence and complete comparison/build/behavior logs are retained in:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration170-u250-target-matrix/nic-packets/
```

The next smallest step is allocating the NIC's 512-bit CPU streams alongside
TracerV, with the oracle's 3,072-entry queues and distinct stream indices,
before binding the NIC MMIO bank and driver collateral. Full-target behavioral
comparison remains necessary after those boundaries are connected.

The archived NIC target reaches both new adapter modules in `replay`.
`nic-candidate-adapter-comparison.json` confirms that their complete module
bodies and ports match the native fixture used in the SFC RTL differential.
`nic-annotation-transfer.json` records preservation of all 1,792 annotation
records after the explicit circuit/copied-port target transfer. The NIC
compile fails specifically on CPU stream allocation and MMIO binding. Its
nine BridgeIO and 58 channel connection records at bridge extraction match
SFC; the later SRAM model-port mismatch remains unresolved.

`no-nic-rtl-comparison.json` confirms that the single-core non-NIC Rocket RTL
is unchanged from iteration 169 (SHA256
`88b4a4760c0055046e67560e30294622d46bce18e426b373118d7dfbd586712a`).
`immutable-rocket-comparison.json` rechecks the exact 2026-10-01 immutable
`.sfc.fir`/`.anno.json` input hashes and U250 build-tree synthesis and
implementation XDC. Normalized constraints match, with the same recorded
`target/FireSim_` to `target_FAMETop/FireSim` implementation hierarchy rename.

The complete thirteen-recipe replay uses compiler SHA256
`6f91e8f9b9e26770c7f568ce3176220ec27cf98e86d99d716f46e248e3aa27eb`.
`iteration170-u250-target-matrix/replay/matrix.json` and `matrix.md` retain
one compiled target, six lowering failures and six unsupported partition
profiles. All seven base bridge/channel annotation boundaries match SFC.
Shuttle's model-port boundary matches; the other six retain the previously
recorded SRAM differences. The replay correctly exits nonzero. Remaining
failures are multiple/non-Rocket TracerV, FASED outstanding profiles beyond
maxFlight=10, NIC stream/MMIO bindings, and partition/QSFP semantics. These
annotation comparisons do not establish full target behavioral or ABI parity.
No FireSim manager verification gate was initiated by this implementation turn.

## Iteration 171: native NIC CPU stream buffering

`addNICCPUStreamQueues` now follows packet adapter assembly in the actual
baseline compiler. It inserts independent incoming and outgoing 3,072×512
FIFOs, using the CPUManagedStreamEngine synchronous read-address lookahead,
12-bit pointers/counts, no flow-through and no pipelining at full occupancy.
Host reset flushes occupancy without clearing RAM or stopping RAM writes and
read-address capture. The shared queue factory retains the original
6,144-entry TracerV implementation and adds the 3,072-entry variant.

The active compiler verifies and emits `post-fame-nic-cpu-stream-queues.mlir`
and `post-fame-nic-cpu-stream-queues-all.json`. The wrapper preserves stream
names/directions and exposes separate UInt<12> occupancy ports. Buffered port
targets remain on the retained packet-adapter module; copied port targets
move to the new top. Stream indices remain unallocated at this boundary.
The compiler still explicitly rejects NIC AXI stream allocation and MMIO
binding; this is partial compiler progress, not complete NIC support.

`goldengate-nic-cpu-stream-queues` CTest passes verified IR geometry, both
independent instance connections, clock/reset/count wiring, stream identity,
annotation transfer, wrong-width/multiple-NIC/constructor/collision/repeated
pass rejection before mutation, and no-NIC preservation. The existing
`goldengate-tracerv-stream-queue` behavioral CTest passes after the shared
factory change.

`test/Transforms/NICCPUStreamQueuesCompare.py` lowers the actual native queue
IR and compares it with SFC `Queue_52` (outgoing) and `Queue_54` (incoming) in:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Both ready/valid, 12-bit count and 512-bit RAM payload comparisons pass over
40,000 standalone Verilator cycles per direction, with independent accepted
transaction scoreboards. Coverage includes 58 full and 7,191 empty cycles,
42,171 simultaneous push/pop cycles, 250 resets, 58,309 pushes, 56,064 pops,
20 pointer wraps, 192 writes during reset, two full-queue pops without a
simultaneous enqueue, and 1,219 empty-queue pushes without bypass. Invalid
payload comparisons also check retained RAM and read-address behavior.

Evidence and complete fixture/lowering/Verilator logs are retained under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration171-u250-target-matrix/nic-cpu-queues-final/
```

The next smallest porting step is deriving a combined live CPU stream catalog
before binding AXI reads/count MMIO and adding incoming AXI writes. The NIC
SFC oracle assigns NIC to-host stream index 0 and TracerV index 1, selecting
AR address bits `[63:19]`; incoming NIC index 0 selects AW bits `[63:18]`.
The existing Rocket-only catalog assigns TracerV index 0, so adding buffering
alone cannot authorize NIC transport or driver collateral. NIC MMIO and
full-target behavioral/ABI comparison remain subsequent work.

The actual archived NIC candidate reaches both independent queue instances.
`nic-candidate-queue-comparison.json` confirms that its entire native queue
module matches the fixture used in the SFC differential, all prior module
bodies survive unchanged, and the live stream/count/host clock/reset wiring
matches the intended boundary. `nic-annotation-transfer.json` confirms
preservation of all 1,792 annotation records after explicit circuit/copied
port target transfer. These reports live at the iteration evidence root.

`no-nic-rtl-comparison.json` records identical final non-NIC Rocket RTL versus
iteration 170, SHA256
`88b4a4760c0055046e67560e30294622d46bce18e426b373118d7dfbd586712a`.
`immutable-rocket-comparison.json` rechecks the exact 2026-10-01 compiler
fixture `.sfc.fir`/`.anno.json` input hashes and U250 build-tree synthesis and
implementation XDC. Normalized constraints match with the recorded
`target/FireSim_` to `target_FAMETop/FireSim` implementation hierarchy rename.

All 13 archived targets were replayed with the final compiler SHA256
`19a017a21df52ac50f115880ea33b8446cfd225464a6948ca3e056cd3ee7c0af`.
`iteration171-u250-target-matrix/replay/matrix.json` and `matrix.md` record
one compiled target, six lowering failures, and six unsupported partition
profiles, each retaining its exact archived input and effective platform.
All seven reached bridge-extraction annotation boundaries match SFC; Shuttle's
model-port boundary matches, while the other six retain the recorded SRAM
differences. The replay exits nonzero for these implementation gaps. NIC now
reaches CPU stream buffering and still rejects stream allocation/MMIO binding.
Multiple/non-Rocket TracerV, FASED profiles beyond maxFlight=10, and partition/
QSFP semantics remain unsupported. Annotation matches do not establish target
behavioral or ABI parity. No FireSim manager verification gate was initiated.

## Iteration 172: live NIC/TracerV CPU catalog and shared reads

The active compiler now constructs NIC token queues, packet adapters and CPU
stream queues immediately after the TracerV buffer, before CPU transport
allocation. `deriveCPUStreamPorts` follows typed wrapper connections and
checks payload/count identity against the same full queue instance path. It
reads NIC metadata from instances rather than the shared queue module, checks
3072×512 storage and independent directions, and preserves IR and all output
lists on rejection. The existing Rocket-only catalog remains available for
other callers and validates the retained 6144×512 TracerV queue.

The combined outgoing order is NIC index 0 and TracerV index 1, with 19 address
selection bits and DMA bases 0 and 524288. Incoming NIC has its own index 0
in the separate sink list. The actual compiler binds both outgoing streams to
native AXI AR/R operations and allocates live count words in source-before-sink
order: outgoing NIC at relative offset 0, TracerV at 4, incoming NIC at 8.
The three-word count bank is emitted before an explicit failure:
`NIC incoming AXI writes and MMIO control binding are not implemented`.
Incoming DMA is enumerated but not bound. Absolute MMIO addresses, complete
NIC widget controls, platform assembly and driver emission remain unsupported.

Focused CTests passed: CPU read, multi-source read, count bank, NIC token
queues, NIC packet adapters, NIC CPU queues and the new CPU stream catalog.
The catalog test traverses nested incoming/outgoing wrappers and verifies
constructor ordering, read window allocation, count offsets, and atomic
rejection of swapped occupancy paths, missing/wrong instance metadata and
wrong stream direction. An initial fixture parse failure was corrected; the
catalog CTest passed after rebuilding. Existing multi-source read behavioral
tests exercise held AR bursts and the SFC rule that every source counter
updates on the shared R handshake, including unselected streams.

`test/Transforms/NICCPUStreamReadCompare.py` extracts the actual candidate's
`GGCPUStreamRead` module, lowers it with firtool, and compares its RTL against
the read-expression cone and two original clocked counters projected from:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h
```

The RTL SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
SFC prunes the constant OKAY response into downstream AXI-buffer queue storage;
the comparison checks that original storage write value before specializing
the projected response. Allocation names/depths/bases match the header. The
header's absolute count addresses are 592, 596 and 600, confirming the relative
0/4/8 order; native absolute address binding is not claimed.

The standalone Verilator comparison passed 50,000 cycles with independent
512-bit queue stimuli, both stream grants, unmapped addresses, reset, held AR
periods and arbitrary address/length changes. Coverage includes 7,738 accepted
responses (3,849 NIC and 3,889 TracerV), 7,896 stalled responses, 12,478 unmapped
address cycles and 170 accepted last beats. Ready/valid and response ID/OKAY
match; payload and last match when R.valid is asserted. SFC's invalid-cycle
payload/last defaults differ from native zero defaults and are intentionally
excluded by the AXI validity contract. This proves the read cone, not complete
NIC simulation or buffered platform timing.

Evidence and complete compiler/lowering/Verilator logs are under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration172-u250-target-matrix/
```

`read-production/comparison.json` compares the real archived-input candidate's
`post-fame-cpu-stream-count.mlir` with those SFC files.
`nic-candidate-binding-comparison.json` confirms every prior NIC module body
is retained and both new read/count helper bodies equal the verified catalog
fixture. `no-nic-rtl-comparison.json` confirms unchanged non-NIC Rocket RTL,
SHA256 `88b4a4760c0055046e67560e30294622d46bce18e426b373118d7dfbd586712a`.
`immutable-rocket-comparison.json` rechecks the exact October 1 immutable
compiler input FIRRTL/annotations and U250 build-tree synthesis/implementation
XDC. Constraints match after the recorded implementation hierarchy rename
`target/FireSim_` to `target_FAMETop/FireSim`.

The next smallest step is native incoming NIC AXI AW/W/B transport with
18-bit window selection, a grant-qualified write counter and depth-3072 FIFO
backpressure. After that, bind NIC widget controls and source/sink count MMIO
through the shared control fabric and derive both driver direction lists from
live allocations. Multiple/non-Rocket TracerV, broader FASED and partition/QSFP
profiles remain matrix gaps. No FireSim manager verification gate was initiated.

The complete thirteen-recipe replay used compiler SHA256
`313feb5d9719347f4e83c9d7550bc24580b5ab354ca574a048a79c91cc7e6962`.
`replay/matrix.json` and `matrix.md` retain one compiled target, six lowering
failures and six unsupported partition profiles. All seven reached
bridge-extraction boundaries match SFC annotation identities. Shuttle's
model-port annotations match; the other six retain the existing SRAM
annotation differences. The matrix exits nonzero and does not claim semantic
parity for any rejected target. `nic-annotation-transfer.json` additionally
checks all 1,792 records and exact recursive copied/consumed target transfer
through both new live read/count wrappers.

## Iteration 173: native incoming NIC AW/W/B transport

`CPUStreamWrite.cpp` ports `CPUManagedStreamEngine.elaborateFromHostCPUStream`
through FIRRTL operations. The active compiler consumes the ordered incoming
catalog after the live source-before-sink count bank and emits
`post-fame-cpu-stream-write.mlir` and its annotations. The NIC sink has index 0,
base 0, depth 3072, width 64 bytes and an independent 18-bit address window.
AW completes only on the final W beat; B.valid excludes B.ready, stream.valid
excludes FIFO readiness, W transfers require B readiness, and only a granted
sink's nine-bit counter advances. B ID and stream data remain live inputs.
Valid-qualified beat-size/strobe assertions match the Scala oracle.

The pass validates identities, directions, types, address allocations, helper
collisions and retained annotations before mutation. It preserves other top
ports and retargets copied-port annotations to the wrapper while consumed sink
annotations retain their inner module identity. The real NIC candidate's
1,792 annotation records match that exact recursive transfer; 53 original
ports survive and `nic_from_host_stream` is consumed.

Nine focused CTests pass: CPU read, multi-source read, empty write, count bank,
NIC token queues, packet adapters, CPU queues, catalog and the new write test.
The write test checks 394,364 SSA cases, 39,104 W transfers and 332 completions,
including independent counters, all 256 held burst lengths, stalls, address
misses, resets, all strobe lanes, live 512-bit data and atomic preflight rejects.

`test/Transforms/NICCPUStreamWriteCompare.py` compares the actual archived-input
candidate helper with original expressions and the clocked write counter
projected from these exact SFC artifacts:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h
```

The RTL SHA256 remains
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The comparison checks SFC's pruned OKAY response value in the original AXI
response queue before specializing that field in the projection. All seven
outputs match on every cycle, including invalid-cycle ID/data. Standalone
Verilator passes 214,188 cycles: all 256 held bursts plus 50,000 randomized
cycles, 34,194 transfers, 122,102 stalled writes, 14,172 misses and 262 accepted
completions. Directed bursts hold AW metadata until completion and W data/valid
until acceptance; the randomized phase also probes arbitrary input changes. Allocation identity/depth/width/base match the SFC header;
absolute count MMIO address binding is still outside this change.

Evidence is under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration173-u250-target-matrix/
```

`write-held-production/comparison.json` records that behavioral comparison, and
`nic-write-annotation-transfer.json` records the complete annotation transfer.
The first production candidate uses a snapshot of the initially linked native
compiler, keeping the comparison independent of a subsequent diagnostic-only
rebuild. `final-write-candidate-comparison.json` confirms the final compiler
replay emits byte-identical write-boundary FIRRTL and all annotations, so both
behavioral and annotation comparisons apply to that final candidate.

The NIC recipe remains an explicit failure after this new boundary:
`NIC buffered CPU AXI transport, MMIO control binding and platform integration
are not implemented`. No completed NIC simulator or driver ABI parity is
claimed. The next smallest step is to attach and compare the existing native
AW/W ingress and B response buffers on this live NIC write boundary, then bind
NIC widget and stream-count controls and derive both driver direction lists.
Other TracerV/FASED and partition/QSFP matrix gaps remain. No FireSim manager
verification gate was initiated.

The final thirteen-recipe replay uses compiler SHA256
`a7eff7f24008e13a8893d891de46db704879da663d6c6b9bea19046682277cb8`. `replay/matrix.json` retains one
compiled target, six lowering failures and six unsupported partition profiles;
the matrix intentionally exits nonzero. All seven reached bridge-extraction
boundaries match SFC. Shuttle model-port annotations match; the other six
retain the existing SRAM annotation differences and do not establish parity.
`no-nic-rtl-collateral-comparison.json` confirms unchanged non-NIC Rocket RTL
and driver header. `immutable-rocket-comparison.json` rechecks the exact
October 1 immutable FIRRTL/annotation inputs and U250 build-tree synthesis and
implementation XDC, matching constraints after the recorded hierarchy rename
`target/FireSim_` to `target_FAMETop/FireSim`. Both immutable fixtures remain
untouched.


## Iteration 174: native live-sink AW/W ingress and B response buffers

The live NIC write path now attaches independent depth-two AW/W queues and the
native depth-two B queue before reporting the remaining NIC integration gap.
`CPUStreamWriteBuffer.cpp` accepts the complete live AW address/length and
512-bit W data input group, retains it in native FIRRTL memories, and routes
both payload and ready/valid through those queues. The empty-sink allocation
and unused-field pruning are preserved. Partial fields, wrong widths and
wrong directions reject before mutation. AW/W targets retain their engine
module identity; B targets retain their pre-response-buffer identity; copied
ports move to the active wrapper.

Six focused CTests pass (empty write, write, AW/W buffer, AR buffer, B buffer,
and NIC CPU queues). The live composition test also checks both directions
of every new payload wire, all 17 fixture port identities, nine atomic live
preflight rejects and repeated-call rejection. The final standalone comparison
uses the actual archived-input compiler candidate and the same attachment APIs;
all four helper module bodies match the production operations exactly, and
all 1,792 production annotation records match the expected recursive transfer.

`test/Transforms/NICCPUStreamWriteBufferedCompare.py` retains the original SFC
`AXI4Buffer_2`, `Queue_55`, `Queue_56`, `Queue_57`, `Queue_59`, and the incoming
write-engine cone from this exact immutable archive artifact:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Its SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The same directory's `FireSim-generated.const.h` supplies allocation checks.
Read pins on the reference buffer are inactive. Native and SFC AW/W ready,
B valid/ID/response, and stream valid/data match through 133,558 cycles:
all 256 burst lengths, six overlapping single-beat requests, 50,000 randomized
cycles, 50,767 stream transfers, 729 accepted AWs, 375 accepted Bs,
23,442 W stalls, 340 B stalls and 180 reset cycles. Payload comparisons are
qualified by valid, since uninitialized queue RAM has no invalid-cycle ABI.
The unbuffered write-cone comparison also passes independently.

Evidence is in:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration174-u250-target-matrix/
```

`buffered-production-checked/comparison.json` records the successful buffered
comparison and full production annotation count. The first two diagnostic
attempts stopped on synthetic test annotations during lowering and a Verilator
post-increment testbench bug; the final test omits only the already-checked
fixture annotations before ordinary lowering and uses an explicit timeout
increment. No native semantic change was required for either test issue.
`no-nic-rtl-collateral-comparison.json` confirms unchanged no-NIC Rocket RTL
and driver header. `immutable-rocket-comparison.json` rechecks the October 1
immutable handoff inputs and U250 build-tree XDC, with implementation clocks
matching after the recorded `target/FireSim_` to `target_FAMETop/FireSim` rename.

NIC compilation remains an explicit failure after the new boundary:
`NIC CPU AR/R buffering, MMIO control binding and platform integration are
not implemented`. Next attach the AR/R buffers to the multi-source NIC read
path, compare combined channel stalls, then bind the NIC and stream-count
MMIO controls and derive both driver direction lists. Complete NIC simulator
execution and driver ABI parity are still pending. No FireSim manager gate
was initiated by this implementation turn.

The fresh thirteen-recipe replay completed with compiler SHA256
`411a232e73cd43d2b29ec76068f3c483458cafd737e0e71533f17f43c8245330`: one compiled no-NIC Rocket target, six lowering
failures and six unsupported partition profiles. All seven reached bridge
annotation boundaries match SFC; Shuttle model-port annotations match, while
the other six retain the existing SRAM annotation differences. The replay
exits nonzero and does not claim semantic parity for rejected targets. NIC's
error advances from its prior buffered-transport gap to the explicit AR/R,
MMIO and platform gap above.

## Iteration 175: buffered NIC CPU reads alongside live writes

The NIC compiler now attaches the native AR and R buffers after its AW/W/B
boundary. AR uses a separate instance of the shared depth-two address queue;
R stores complete ID/512-bit data/last/response beats. Both queues are
non-flowing and non-pipelined. The source engine advances at R enqueue even
when the host stalls an older response. Copied annotation targets move outward;
consumed AR and R targets retain their respective inner module identities.
The compiler verifies and emits `post-fame-nic-cpu-buffered.mlir` and its complete
annotation archive before rejecting the remaining NIC MMIO/platform gap.

`test/Transforms/NICCPUStreamBufferedCompare.py` composes the exact production
read/write helpers using the same four attachment calls as the compiler. It
checks six identical helper bodies, all production wrapper routes, all 67
public ports, 72 unchanged prior module bodies and all 1,792 annotation records.
The composition executable checks all 31 fixture port identities, six atomic
AR/R rejects and the existing nine live AW/W rejects. Independent unbuffered
read and write comparisons also pass.

The behavioral reference retains SFC `AXI4Buffer_2`, `Queue_55`, `Queue_56`,
`Queue_57`, `Queue_59`, and both original CPUManagedStreamEngine cones from:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Golden RTL SHA256:
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The same directory's `FireSim-generated.const.h` validates NIC and TracerV
stream allocation and count register ordering. Ready/valid, valid-qualified
response payloads and source handshakes match for 226,688 cycles: 256 write
burst lengths, 512 read bursts across both outgoing sources, six queued read
requests, 50,000 randomized cycles, 50,499 write stream transfers, 74,075 host
read beats, 15,245 concurrent read/write transfers, 73,679 R stalls, 24,642 AR
stalls and 194 reset cycles. Invalid queue payloads have no specified ABI and
are not compared. This standalone unit simulation does not run a manager gate.

Evidence is under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration175-u250-target-matrix/
```

`duplex-production/comparison.json` records the behavioral and structured
comparison. It uses a snapshot of the first linked compiler while the final
build incorporates generalized preflight diagnostics.
`final-nic-candidate-comparison.json` confirms the final archived-input replay
emits identical write, AW/W/B and full buffered FIRRTL and annotation archives;
the behavioral comparison therefore applies to the final compiler. Its SHA256
is `decef254cf17f0281adc20e207ecf4eb4b668b3e80b1ecdfd224341033e1fa8c`.
The rebuilt composition executable also emits identical fixture FIRRTL. Eight
focused CTests pass: empty write, live write, all four buffer passes, multi-source
read, and NIC CPU queues. `no-nic-rtl-collateral-comparison.json` confirms the
no-NIC Rocket RTL, driver header, `.const.vh` and both XDC files are unchanged.
`immutable-rocket-comparison.json` rechecks the exact October 1 FIRRTL/annotation
handoff and U250 XDC, including the recorded implementation hierarchy rename.

NIC compilation still fails explicitly with `NIC MMIO control binding and
platform integration are not implemented`. The next smallest step is to bind
NIC widget controls to SFC MMIO addresses 512/516/520/524/528/532 and the three
stream-count registers to 592/596/600, then derive incoming and outgoing driver
allocation lists. Complete
NIC simulator/runtime parity, other TracerV/FASED gaps, and partitioned QSFP
support remain pending. The October 1 immutable fixtures remain read-only.

The final thirteen-recipe replay completes with one compiled target, six
lowering failures and six unsupported partition profiles, using the final
compiler SHA256 above. All seven reached bridge-extraction annotation
boundaries match SFC; Shuttle model-port annotations match, while the other six
retain the existing SRAM annotation differences. The replay intentionally
exits nonzero. NIC advances from its AR/R gap to the explicit MMIO/platform gap;
other target gaps persist. No complete NIC simulator or driver ABI parity is
claimed, and no FireSim manager verification step was initiated.

## Iteration 176: native decoded NIC MMIO registers

The production NIC path now attaches `GGNICMMIOBank` after its buffered duplex
CPU transport. Native FIRRTL registers, muxes, slices, concatenation, connects
and assertions implement the normal-mode SimpleNICBridge configuration bank.
The five write-only 32-bit configuration registers remain unreset. Their
updates ignore byte strobes and host reset; target MAC, limiter and pauser
outputs use the SFC bit slices. The sixth register, `done`, remains an unreset
one-bit sample of the existing `!tFire` signal. A decoded write overrides the
sample for that cycle, as `Widget.genROReg` and `MCRIO.bindReg` do in SFC.
Reads return zero for the five write-only words and zero-extended `done` for
the sixth word; all read-valid and write-ready signals are constant one.
Host reset disables only the five write-only read assertions.

The new wrapper consumes seven configuration inputs and the combinational
done output, retains all other public ports and recursively transfers copied
annotation targets. `post-fame-nic-mmio.mlir` and its complete annotation
archive are emitted by the actual compiler, then compilation stops explicitly
at `NIC Nasti control binding and platform integration are not implemented`.
The bank's ordered register metadata records local offsets 0/4/8/12/16/20,
permissions and names; global allocation is still pending.

`test/Transforms/NICMMIOBankTest.cpp` verifies six unreset registers, five
assertions, permissions, active control/status routes, nested identities and
29 preflight failures with no IR mutation. Eleven focused CTests pass,
including NIC token/packet/CPU queues, stream catalog, all four CPU transport
buffers, multi-source reads, live writes and the new MMIO bank.

`test/Transforms/NICMMIOBankCompare.py` applies the same native attachment to
the fresh production boundary, requires identical module bodies, checks all
60 resulting public ports and all wrapper connections, preserves all 75
prior module bodies and compares all 1,792 annotation records. It checks
the five native assertion predicates and reset enables against the original
SFC register-cone guards. The behavioral reference retains actual SFC state
declarations, combinational assignments and clocked updates from:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Golden RTL SHA256:
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The same directory's `FireSim-generated.const.h`, SHA256
`ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`,
confirms register order and SFC global addresses 512/516/520/524/528/532;
these addresses are reference requirements, not yet a mapped native decoder.
Only decoded MCR inputs and the already-compared combinational done signal
replace SFC boundaries in the standalone reference. Verilator with assertions
enabled matches all configuration outputs and register read data for 54,097
cycles: all 64 write masks, 16 strobes and four reset/done combinations,
followed by 50,000 randomized cycles. Coverage includes 13,538 reset-time
writes, 1,613 zero-strobe writes, 27,146 done overrides and 26,951 live done
samples. Uninitialized register values are unspecified and are initialized
by decoded writes before comparison. This is a unit comparison, not NIC
simulator/runtime parity.

Evidence is under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration176-u250-target-matrix/
```

`mmio-production/comparison.json` records the exact reference and candidate
hashes and comparison result. `no-nic-rtl-collateral-comparison.json` confirms
unchanged no-NIC Rocket RTL, driver header, `.const.vh` and both XDC files.
`immutable-rocket-comparison.json` freshly verifies the October 1 immutable
FIRRTL/annotation handoff and U250 XDC, including the previously recorded
implementation hierarchy rename. Both immutable fixtures remain read-only.

Next bind this six-word bank to native Nasti MCRFile transactions, then map
NIC and the three stream-count registers into the host control allocation.
Driver stream lists and complete NIC platform/runtime integration remain
pending, along with other target lowering gaps and partitioned QSFP support.
No FireSim manager verification step was initiated by this implementation.

The fresh thirteen-recipe replay uses compiler SHA256
`696f52e0d38bd98033cb7f62699ceb5bdaf66660b6fd122178a77b8da9761916`.
It completes with one compiled no-NIC Rocket, six lowering failures and six
unsupported partition profiles, and intentionally exits nonzero. All seven
reached bridge/channel annotation boundaries match SFC. Shuttle model-port
annotations match; the other six retain the existing SRAM annotation
differences. The failures remain: quad-core multiple TracerV constructors;
BOOM and Shuttle TracerV payload profiles; Gemmini and Saturn Rocket FASED
maxFlight profiles; NIC Nasti/platform integration; and all six partitioned
QSFP profiles. Only the NIC failure advances to the new decoded-MMIO boundary.

## Iteration 177: native NIC Nasti control transport

The actual NIC compiler path now binds its six-word decoded registry/bank to
native FIRRTL `GGNICMCRFile` operations through the shared Lib.scala MCRFile
implementation. The registry determines the word count and three-bit index.
Independent AW/W capture, held B responses, single outstanding AR/R, captured
IDs/data, host-reset transaction flags, single-beat assertions and SFC address
aliasing now drive the previously attached NIC configuration bank. Byte strobes,
W.last and unused Nasti metadata retain the SFC behavior. Local holes six and
seven never commit a write; their reads follow SFC's default word-zero mux.

`GGNICBridgeControlWrapper` replaces the public decoded bank with a Nasti slave,
retains all other public ports and recursively transfers copied annotation
identities. The compiler emits `post-fame-nic-control.mlir` and
`post-fame-nic-control-all.json`, then explicitly rejects the next boundary:
`NIC global control allocation and platform integration are not implemented`.
This step does not claim a complete NIC target compile or runtime parity.

`test/Transforms/NICControlTest.cpp` checks production routes, six-word geometry,
four reset flags, five unreset captures, two assertions, recursive targets and
12 preflight rejections without IR mutation. Its standalone mode clones the
actual emitted MCRFile and bank bodies. Twelve focused CTests pass, covering
control transport/decode, all four CPU stream buffer passes, NIC token/packet/
CPU queues, stream catalog and decoded MMIO. The final standalone composition
and rejection test also passes after rebuilding the test executable.

`test/Transforms/NICControlCompare.py` reproduces the fresh production attachment,
requires identical module bodies, preserves all 77 prior module bodies, checks
all 60 public ports and wrapper connections, and compares all 1,792 recursively
transferred annotation records. Its behavioral reference retains the actual
SFC `MCRFile_4` and the six-register cone from:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Golden SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Only the previously compared combinational done boundary replaces SFC `!tFire`.
Verilator with assertions enabled matches the native production helpers for
103,828 cycles: all eight local indices, three AW/W arrival orders, 16 byte
strobes, address aliases, write-only read guards, live done under R stalls,
96 hole writes recovered by reset, and 100,000 randomized cycles. Coverage
includes 3,922 AW handshakes, 25,308 AR handshakes, 3,716 B stalls, 24,709 R
stalls, 1,094 resets with pending captures, and 229 zero-strobe W handshakes.
Uninitialized config/capture values remain unspecified; configuration is
initialized through the same Nasti transport before comparison. This isolated
unit comparison does not initiate a FireSim manager verification gate.

Evidence is under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration177-u250-target-matrix/
```

`control-production/comparison.json` records the golden/candidate hashes and
behavioral result. `no-nic-rtl-collateral-comparison.json` freshly confirms
unchanged no-NIC Rocket RTL, driver header, `.const.vh` and both XDC files.
`immutable-rocket-comparison.json` freshly checks the October 1 FIRRTL/annotation
handoff and U250 XDC, including the recorded implementation hierarchy rename.
Neither immutable fixture was modified.

Next derive the NIC and three stream-count widgets' global allocation from the
native MMIO registries, compare offsets 512/516/520/524/528/532 and 592/596/600
against the NIC SFC driver header, and attach their native control decoder.
Driver stream lists and complete NIC platform/runtime integration remain
pending, together with the other target lowering gaps and partitioned QSFP.

The fresh thirteen-recipe replay uses compiler SHA256
`9560ac2c2830f9fb87309d487d1a87d5bd7dc4d593cf3336f15de95b83a2627f`.
It completes with one compiled no-NIC Rocket, six lowering failures and six
unsupported partition profiles, intentionally exiting nonzero. All seven
reached bridge/channel annotation boundaries match SFC. Shuttle model-port
annotations match; the other six retain the existing SRAM annotation
differences. Quad-core multiple TracerV, BOOM/Shuttle TracerV payload profiles,
Gemmini/Saturn Rocket FASED maxFlight profiles and six partitioned QSFP profiles
remain unsupported. NIC advances specifically to its global control/platform
allocation gap. Matched intermediate annotations are partial evidence, not
complete target passes. The live build includes the user's pre-existing UART
registry-sizing change; that unrelated change is excluded from this checkpoint.

## Iteration 178: native NIC CPU stream occupancy control

The production NIC pipeline now attaches the implemented CPU stream count bank
through native FIRRTL MCRFile operations after its NIC control/CPU DMA wrappers.
`attachCPUStreamControl` accepts explicit wrapper identities and derives the
transport geometry from `GGCPUStreamCountBank`'s register registry, validating
its actual MCR lanes before creating operations or transferring targets. The
three-word NIC bank uses two-bit indices and retains the shared Scala MCRFile
AW/W capture, AR/R handling, reset flags, response IDs and local address aliases.
The new top replaces `cpuStream_mcr` with `cpuStream_ctrl`, preserves other ports
and recursively transfers copied annotation targets. The compiler emits
`post-fame-nic-cpu-control.mlir` and `post-fame-nic-cpu-control-all.json`, then
explicitly rejects the remaining global control allocation/platform boundary.
This is an incremental local attachment, not a complete NIC target compile.

Four focused CTests pass: shared clock/bridge MCR control, ordered CPU stream
count bank, NIC control and the new NIC CPU control attachment. The new test
checks two-bit indices, standalone production cloning, nested annotation
identity and eleven invalid bindings rejected without IR mutation. Its final
message-only revision was rebuilt and both fixture/production modes rerun;
all helper and wrapper module bodies remain identical to the behavioral test.

`test/Transforms/NICCPUStreamControlCompare.py` compares the actual production
attachment and its cloned count bank/MCRFile against:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h
```

The RTL golden SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
the driver header SHA256 is
`ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
The reference retains SFC `MCRFile` verbatim and isolates the live occupancy
inputs at the original CPUManagedStreamEngine queue-count assignments. It
checks the exact ordered registry: NIC outgoing, TracerV outgoing, NIC incoming,
12/13/12-bit counts, local offsets 0/4/8 and read-only permissions. SFC global
addresses 592/596/600 are recorded, not claimed as implemented global routing.
All 79 prior module bodies, 60 public ports/routes and 1,792 transferred
annotations match. Verilator with assertions enabled matches for 100,721 cycles:
25,210 reads, 25,403 read stalls, 1,111 reset cycles, 48 uncommitted hole writes,
all four indices and low-address aliases, three AW/W arrival orders, sixteen
strobes and 100,000 randomized cycles. Counts remain live under R backpressure.
This isolates local occupancy/control behavior; full DMA queue/driver/platform
integration and illegal read-only write handling are not new behavioral claims.

Evidence is under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration178-u250-target-matrix/
```

`cpu-control-production-final/comparison.json` records the candidate/golden
hashes and structured/behavioral results. Fresh no-NIC RTL, both constant headers
and both XDC files are byte-identical to iteration 177. Fresh immutable October 1
FIRRTL/annotation inputs and U250 XDC comparisons match, with the recorded
implementation hierarchy rename. No golden fixture was modified.

Next generalize the native widget catalog/allocation for the NIC and its
three-word CPU stream bank, then route their control slaves and compare the
resulting global addresses against the SFC driver. Complete NIC platform/runtime,
other target profiles and partitioned QSFP remain pending. The live compiler
includes the user's pre-existing UART registry-sizing edits; those edits are
excluded from this checkpoint. FireSim manager gates remain harness-owned.

The fresh thirteen-target replay uses compiler SHA256
`5b25ac9e0d441aaf180e99b99c9dee3a9bdedbea0f008a98b494f3658130f778`.
It completes with one compiled no-NIC Rocket, six lowering failures and six
unsupported partition profiles, intentionally returning nonzero. All seven
reached bridge/channel boundaries match SFC; Shuttle model-port annotations
match and the other six retain the existing SRAM annotation differences.
Multiple TracerV constructors, BOOM/Shuttle trace payloads, Gemmini/Saturn Rocket
FASED maxFlight profiles and six partitioned QSFP profiles remain gaps. NIC
reaches the new CPU occupancy transport boundary before its explicit global
allocation/platform rejection. Partial boundary matches and compile-only
results are not complete semantic passes.

## Iteration 179: shared native LoadMem path for NIC transport

The NIC CPU control boundary now enters the shared CIRCT LoadMem pipeline.
`attachLoadMemWriter` binds an explicit active, uninstantiated host top while
preserving the existing no-NIC entry point and all preflight checks. Empty or
mismatched identities, missing annotations/clock/reset, instantiated tops and
colliding helper/port names reject before mutation. The compiler keeps the
no-NIC CPU transport construction in its own branch, then shares LoadMem writer,
request queue/arbiter, write MMIO/data FIFO, read requests/data FIFO and MCRFile
operations with NIC. The NIC replay also reaches native SimulationMaster, TSI,
BlockDev and FASED bank materialization. It still explicitly rejects immediately
before global control allocation/platform integration. This does not constitute
a complete NIC target compile or host memory integration.

The final compiler and writer test build passes. All six focused CTests pass:
`goldengate-loadmem-writer`, `goldengate-loadmem-requests`,
`goldengate-loadmem-write-mmio`, `goldengate-loadmem-write-data`,
`goldengate-loadmem-read-requests` and `goldengate-loadmem-read-data`.
The writer test covers both active top identities, nested target transfer and
ten atomic rejection cases. Its export mode clones the actual production
`GGLoadMemWriter` operations for the RTL differential test.

`test/Transforms/NICLoadMemCompare.py` compares the production NIC attachment
against these exact SFC artifacts:

```
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Their SHA256 values are, respectively,
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`
and `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.
Both contain identical cleaned `LoadMemWriter` module operations. The comparison
retains that SFC module verbatim and lowers the cloned production writer with
firtool. Native/SFC ports and the NIC widget's addr34/data64/maxBurst32 geometry
match. All 81 prior module bodies, 60 copied public ports/routes, 18 new writer
ports and 1,792 transferred annotation records match. Eight shared LoadMem
helper bodies match the fresh no-NIC replay.

Verilator with assertions enabled matches native and SFC writer outputs before
and after every edge for 115,918 cycles: 1,662 requests, 2,263 bursts, 41,575 data
beats, 1,296 responses and 1,025 reset cycles. Directed cases cover all 194
zero/nonzero requests of lengths 0 through 96, zero-length underflow, burst
boundaries, multiburst writes and 34-bit address wrap. Reset exercises all four
FSM stages, followed by 100,000 randomized cycles with independent request,
data, AW, W and B stalls. Unreset AW/W payloads are compared when valid. This
isolates writer behavior; full LoadMem host arbitration and NIC platform/runtime
behavior remain pending.

Evidence lives under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration179-u250-target-matrix/
```

`nic-loadmem-production/comparison.json` records the golden/candidate hashes,
structured checks and behavioral result. Run the comparator with `--before`
pointing to NIC `post-fame-nic-cpu-control.mlir`, `--writer` to NIC
`post-fame-loadmem-writer.mlir`, `--control` to NIC
`post-fame-loadmem-control.mlir`, `--baseline-control` to the no-NIC corresponding
control boundary, both golden paths above, the built writer test as
`--export-tool`, and a new `--output-dir`.

`no-nic-rtl-collateral-comparison.json` freshly records that RTL, both constant
headers and both XDC files are byte-identical to iteration 178.
`immutable-rocket-comparison.json` freshly matches the immutable October 1
FIRRTL/annotation handoff and U250 XDC with the recorded implementation hierarchy
rename. No golden fixture was modified. The live compiler includes the user's
pre-existing UART registry-sizing edits; they are excluded from this checkpoint.

The harness's iteration 178 CIRCT replacertl, Verilator bare/portable/Rocket UART
regressions and U250 buildbitstream passed. Hardware infrasetup subsequently
failed before programming: its FPGA enumeration database was empty and
`slot_to_bdf` rejected `Less FPGAs available than slots (0 >= 0)`. Complete stdout,
stderr and manager log `2026-10-14--05-42-27-infrasetup-YKTSVIUHZVDIR91J.log`
were read. That failure supplies no evidence of an RTL defect or Linux boot.
No manager verification step or hardware recovery was started in this turn.

Next generalize the native widget catalog/allocation for NIC and its ordered
three-word CPU stream bank, bind their control slaves, and compare global
addresses 512/516/520/524/528/532 and 592/596/600 with the NIC SFC driver header.
Host memory arbitration, driver stream lists, full NIC platform/runtime,
other target lowering profiles and partitioned QSFP remain gaps.

The fresh thirteen-target replay uses compiler SHA256
`83cd71aa1bf373ea930d2966454ea24794a4b36c3d1fee2ee363e86c2839cd1f`.
It completes with one compiled no-NIC Rocket, six lowering failures and six
unsupported partition profiles, intentionally returning nonzero. All seven
reached bridge/channel boundaries match SFC; Shuttle model-port annotations
match and the other six retain the existing SRAM annotation differences.
Multiple TracerV constructors, BOOM/Shuttle trace payloads, Gemmini/Saturn Rocket
FASED maxFlight profiles and six partitioned QSFP profiles remain unsupported.
NIC reaches the shared LoadMem/local control and materialized widget banks
before its explicit global allocation/platform rejection. Intermediate boundary
matches are partial evidence, not complete semantic passes.

## Iteration 180: NIC global MMIO allocation and address decoder

The native Rocket allocation boundary now includes the materialized
`GGNICMMIOBank`/`GGNICMCRFile` and validates its duplex CPU stream catalog.
SimpleNIC registers before TracerV/TSI/Clock, preserving its equal-sized tie
with Clock. `CPUManagedStreamEngine.scala` registers all source counts before
sink counts; the native allocator checks NIC source 0, TracerV source 1 and
NIC sink 0 against the three read-only registry words. Missing halves, stale
names/indices/offsets/permissions or truncated count registries fail atomically.
The existing Print and no-NIC allocation path retains its registration order.

The production compiler now emits `post-fame-control-address-decode.mlir` and
its annotations for the NIC target. It then explicitly rejects unsupported
NIC global request/response routing and platform integration. This is a
native FIRRTL decoder boundary, not a complete NIC simulator or ABI emission.

Compared exact archived SFC artifacts:

```
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h
```

RTL SHA256 remains
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
header SHA256 is
`ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
All twelve native region starts, sizes and slave indices match the original
AW and AR address predicates extracted from SFC `NastiRouter`. The NIC's six
register addresses are 512–532; source counts are 592/596 and sink count is
600. All 85 scalar driver addresses lie within their corresponding regions.
The comparison preserves 107 prior module bodies, 101 copied ports and all
1,792 annotation records with recursive circuit/copied-port target transfer.

The control decoder CTest passed 123,462 address pairs and its existing
registry, allocation and IR rejection cases. The new production-input test
passed 20,664 address pairs and ten atomic rejection cases. It exports the
actual decoder from the complete production pre-allocation boundary; the
exported module body matches the fresh compiler's decoder exactly. Isolated
Verilator execution compares that native RTL with the unchanged SFC AW/AR
predicate expressions for 1,148,578 address pairs: every pair of byte addresses
0–1023, 100,000 deterministic random pairs over the 25-bit space, and directed
unmapped/end-of-region endpoints. One-hot routes, decoded target indices and
error selection match. Queued transactions and full platform behavior remain
outside this test.

Evidence lives under:

```
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration180-u250-target-matrix/
```

`nic-control-allocation-production/comparison.json` records hashes, the twelve
regions, structured checks and behavioral results. Run
`test/Transforms/NICControlAllocationCompare.py` with `--before` pointing to
NIC `post-fame-fased-histograms-bank.mlir`, `--decoder` to NIC
`post-fame-control-address-decode.mlir`, `--golden` to the archived NIC SV,
`--export-tool` to `goldengate-control-address-decode-test`, and a fresh
`--output-dir`, after sourcing the FireSim environment.

`no-nic-rtl-collateral-comparison.json` freshly matches RTL, both constant
headers and both XDC files byte-for-byte against iteration 179.
`immutable-rocket-comparison.json` freshly matches the October 1 immutable
primary FIRRTL/annotations and immutable U250 XDC, with the documented
implementation hierarchy rename. No immutable fixture was changed.

Fresh standalone replay used all thirteen archived handoffs and each effective
platform configuration. Compiler SHA256:
`7420c3bc133018a5eb8d1677bb044f4e7d6cb28e0bfa9b21c281933b36213579`.
`replay/matrix.json` records one complete compile, six later lowering failures
and six unsupported partition/QSFP platforms; its nonzero exit remains
intentional. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle's model-port boundary matches; the other six retain exactly the prior
native-SRAM annotation differences. These are partial findings, not target
parity. NIC now stops after address decoding; the other failure reasons remain
unchanged. No FireSim manager verification step was started in this turn.

The next smallest step is to generalize the existing native AW/W request
queues and widget dispatch for the twelve-region NIC catalog, retaining its
three-word CPU stream bank and comparing accepted transactions against SFC.
The live build includes pre-existing user ClockBridgeControl/UART registry
edits; those edits are excluded from this checkpoint.

## Iteration 181: NIC queued AW/W routing and slave dispatch

The production compiler now runs the native `addControlWriteRoute` and
`addControlWriteDispatch` passes on the twelve-region NIC control catalog.
The route queue has twelve entries of twelve bits, four-bit wrapping pointers,
and neither flow nor pipe forwarding. AW acceptance coordinates queue space,
tracker readiness and the selected slave; accepted W.last consumes the saved
route. Native dispatch drives all twelve normal AW/W endpoints and connects
the existing error endpoint. The typed region catalog, including the three
CPU stream count words, is retained.

The NIC compiler emits `post-fame-control-write-route.mlir` and
`post-fame-control-write-dispatch.mlir` plus their complete annotations, then
explicitly rejects NIC widget binding, responses and platform integration.
Its failed compile remains an implementation gap; this boundary is not a
complete NIC simulator. The no-NIC compiler path emits identical collateral.

Compared the exact archived SFC artifact:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256 is `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The test extracts the original `Queue_60` declarations, memory and clocked
updates, plus 115 original `NastiRouter` write-cone expressions. Only the
decoded route, tracker readiness and normal/error slave readiness are test
boundaries. The queue's AW bit input replaces the equivalent low/high
address-decoder concatenation with that same twelve-bit route.
The prior allocation comparison establishes the decoder's SFC region match.
Normal W.last is optimized out of the SFC widget-facing ports; the test checks
its native broadcast directly against the input, and compares the retained
SFC error W.last and queue-dequeue expression.

`ControlWriteRouteTest.cpp` now interprets the actual production queue and
clones the emitted queue/dispatch helpers for isolated RTL lowering, without
rebuilding either helper. Its production mode passed 34,100 cycles against
an independent deque, including reset writes and pointer wrapping. The
focused CTests passed depth-twelve queue cases, 12,705 twelve-slave dispatch
cases, existing other widths, mapping checks and 26 atomic rejection cases.

`NICControlWriteRoutingCompare.py` checks exact production wrapper connections
and recursive annotation transfer: the route boundary preserves 109 prior
module bodies and 109 copied ports with 125 connections; dispatch preserves
111 bodies and 115 copied ports with 241 connections. Both retain all 1,792
annotation records. Exported helper bodies match the compiler output exactly.

Isolated Verilator execution passed 103,801 cycles against the original SFC
queue and request expressions: 14,037 accepted AWs, 10,527 accepted W.lasts,
111 full-queue/pop cases, 13,227 empty-queue W cases, 10,272 held burst beats,
1,173 simultaneous enqueue/dequeue cases and 232 reset writes. All twelve
normal endpoints and the error endpoint were exercised, including multi-bit
route priority and independently selected AW/W routes. Payloads, handshake
coordination and selected valid signals match. Tracker retirement, widget
responses and complete platform behavior remain outside this test.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration181-u250-target-matrix/
```

`nic-control-write-routing-production/comparison.json` records structured and
behavioral checks and artifact hashes. Run the comparison after sourcing the
FireSim environment, with `--decoder`, `--route` and `--dispatch` pointing to
the three production NIC MLIR boundaries, `--golden` to the archived NIC SV,
`--export-tool` to `goldengate-control-write-route-test`, and a fresh
`--output-dir`. The initial exporter-only failure log is retained separately;
the exporter now builds its isolated circuit before verification.

`no-nic-rtl-collateral-comparison.json` freshly matches the no-NIC RTL, constant
headers and both XDC files against iteration 180 byte-for-byte.
`immutable-rocket-comparison.json` freshly matches October 1 primary
FIRRTL/annotations and U250 XDC with the documented implementation hierarchy
rename. Neither immutable fixture was changed.

The next smallest step is to bind the twelve write-dispatch endpoints to the
typed widget MCR banks, including NIC's six registers and the three-word
read-only CPU count bank. Then port read dispatch and response coordination
for this catalog. Host memory arbitration, driver stream lists, full NIC
platform/runtime, other target lowering profiles and partitioned QSFP remain
gaps. Pre-existing user ClockBridgeControl/UART edits are excluded from this
checkpoint. No FireSim manager verification step was started in this turn.

Fresh standalone replay of all thirteen archived handoffs and their effective
platform configurations used compiler SHA256
`462b005e0fcc45f6ea197b45716192275875e570e2fb061c5fac80a7a090fe29`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations; nonzero exit is
intentional. All seven reached BridgeIO/channel boundaries match SFC. Shuttle
model-port annotations match; the other six retain exactly the prior SRAM
annotation differences. `matrix-regression-comparison.json` checks unchanged
inputs, effective platforms, statuses and all structured boundary results
against iteration 180. NIC advances to write dispatch; the other failure
reasons remain unchanged. Its fresh decoder MLIR and annotations are also
byte-identical to iteration 180's SFC-compared decoder boundary.

## Iteration 182: bind NIC write dispatch to the instantiated MCR bank

The production `bindRocketControlWidgetWrites` pass now includes the allocated
six-word normal-mode NIC bank. It validates the typed MCR registry and follows
the active wrapper's whole-bundle forwarding to the actual `crFile` instance
of `GGNICMCRFile` before changing IR. Detached ports, aliases, duplicate
forwarding, incorrect adapter directions, missing registries and inconsistent
NIC allocation identities reject atomically. Both Nasti and MCR adapter ports
are inputs in the production CIRCT contract.

The pass uses the decoder's allocation rather than assuming baseline slave
indices: NIC is slave 7, Clock is 8, the three-word CPU stream count bank is
10, and Reset is 11. Eight banks now receive scalar AW/W requests and return
readiness internally. Their remaining AR/B/R bundles retain their directions;
eleven shared AW/W metadata inputs retain the original transaction fields.
NIC compilation emits `post-fame-control-widget-writes.mlir` and complete
annotations, then explicitly rejects missing NIC read dispatch, responses and
platform integration. This is still a failed target compile.

Compared this exact SFC golden artifact:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlWidgetWritesCompare.py` matches 64 original `FPGATop` AW/W allocation
connections for all eight bound banks. It checks the exact 330 production
connections, 154 copied ports, preservation of 113 prior module bodies, and
recursive transfer of all 1,792 annotation records. The standalone exporter
clones the actual production NIC bank, adapter, twenty scalar binding
operations and three remaining bundle connections. Its 36 connections match
the corresponding production slice; no request expression is rebuilt.

Isolated Verilator execution compares that slice with unchanged SFC
`MCRFile_4` RTL and the original six-register NIC cone. It passed 103,828
cycles, including 3,922 accepted AWs, 25,308 reads, 3,716 B backpressure cycles,
24,709 R backpressure cycles, 96 unmapped-register reads, 1,094 reset cycles
with pending requests and 229 zero-strobe writes. All three AW/W arrival
orders, sixteen strobes, eight register indices and 100,000 deterministic
random cycles were exercised. NIC done and dispatch/read/response interfaces
are explicit test boundaries; global trackers and platform behavior are not
covered. The existing direct NIC control comparison also passed the same
103,828-cycle reference after extracting its RTL comparison into a shared
function.

Focused CTests passed the existing three binding layouts (420 exact scalar
connections), the new twelve-region NIC layout (160 scalar connections),
partial annotation targets and forty atomic rejection cases, plus the NIC
control test. Initial adapter-direction and exporter metadata-selection
failures were fixed from production MLIR evidence. Their diagnostic logs are
retained; the corrected fresh matrix and comparison below use stable binaries.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration182-u250-target-matrix/
```

`nic-control-widget-writes-production/comparison.json` records the fresh
candidate/golden hashes and structured/behavioral results. Run the comparison
after sourcing the FireSim environment, with `--dispatch` and `--candidate`
pointing to the production NIC dispatch and widget-write MLIR boundaries,
`--golden` to the archived NIC SV, `--export-tool` to
`goldengate-control-widget-writes-test`, and a fresh `--output-dir`.

`no-nic-rtl-collateral-comparison.json` freshly matches all five no-NIC RTL,
constant-header and XDC artifacts byte-for-byte against iteration 181.
`immutable-rocket-comparison.json` freshly matches the October 1 immutable
primary FIRRTL/annotations and U250 XDC, with the documented implementation
hierarchy rename. No immutable fixture was edited.

The next smallest step is to exercise the existing native AR dispatch with
this eight-bank/twelve-region catalog and compare its accepted read requests
against the SFC router, retaining NIC's six words and CPU's three count words.
Response trackers/arbitration, remaining widget bindings, host memory/driver
stream lists, NIC runtime, other target lowering profiles and partitioned
QSFP remain gaps. Pre-existing user ClockBridgeControl/UART registry edits
are excluded from this checkpoint. No FireSim manager verification step was
started in this turn.

Fresh standalone replay of all thirteen archived handoffs and effective
platform configurations used compiler SHA256
`de786fdf748242b5dd81774cd0c87fb5d530e376573e4c0ed1990c028f58b5b1`.
`replay-final/matrix.json` records one complete no-NIC compile, six later
lowering failures and six unsupported partition/QSFP configurations; its
nonzero exit is intentional. All seven reached BridgeIO/channel boundaries
match SFC. Shuttle model-port annotations match; the other six retain exactly
the prior native-SRAM annotation differences.
`matrix-regression-comparison.json` checks unchanged inputs, effective
platforms, statuses and structured boundary results against iteration 181.
The six NIC decoder, queued write-route and write-dispatch MLIR/annotation
artifacts are byte-identical to those already compared with SFC in iterations
180/181. Only NIC's later unsupported-feature reason advances. The first
replay overlapped the adapter-direction correction and failed its compiler
snapshot guard; it is retained as diagnostics, not validation evidence.
Its large derived artifacts were compressed with verified uncompressed
SHA256s to preserve disk space. Use `replay-final` for the next comparison.

## Iteration 183: dispatch NIC control read requests

The production compiler now invokes `addControlReadDispatch` for the NIC
handoff after binding widget writes. It emits verified
`post-fame-control-read-dispatch.mlir` and complete annotations before
explicitly rejecting the remaining NIC response/platform work. Requests
reach the eight allocated banks, including NIC at slave 7, Clock at 8, CPU
stream at 10 and Reset at 11. Four unbound banks expose AR interfaces; the
error endpoint connects internally. The shared pass derives twelve route
bits and four target bits from the decoder catalog. This is an additional
native compiler boundary, not a successful NIC target compile.

The exact golden artifact compared is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlReadDispatchCompare.py` matches forty original `FPGATop` AR
allocation connections and all 193 production wrapper connections. It
checks preservation of 114 earlier module bodies, the 160 copied ports
(with eight narrowed B/R bundles), and recursive transfer of all 1,792
annotation records. The exporter runs the actual pass on the production
widget-write boundary and clones its emitted helper. Both full production
IR and isolated helper match the compiler's output; no helper expression is
rebuilt for the comparison.

Isolated Verilator execution against seventy unchanged expressions from
SFC `NastiRouter` passes 525,984 cases, including 65,641 master accepts and
196,940 stalls. It covers all thirteen endpoints, all 8,192 slave-ready
masks under all four valid/capacity combinations, and 100,000 deterministic
random cases (including multiple route bits). Master ready, slave valid,
tracker enqueue valid/tag and SFC-retained address/length/ID broadcasts
agree. The other eight native payload fields also broadcast unchanged to
all endpoints; target metadata forwards unchanged from its explicit input.
Decoded route/target and tracker capacity are comparison boundaries. This
does not test response storage, retirement, burst arbitration or platform
behavior. The earlier decoder and write-binding artifacts must remain
unchanged to reuse their recorded SFC evidence.

The focused CTest includes the actual twelve-region NIC allocation, all
ready masks, multi-route priority, partial annotation targets and the
existing malformed/repeated-boundary atomic rejection tests. It passes
alongside the production IR verification and differential comparison.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration183-u250-target-matrix/
```

`nic-control-read-dispatch-production/comparison.json` records exact hashes,
structured checks and behavioral results. After sourcing the FireSim
environment, run `test/Transforms/NICControlReadDispatchCompare.py` with
`--writes`/`--reads` pointing to those production MLIR boundaries, `--golden`
to the archived NIC SV, `--export-tool` to
`goldengate-control-read-dispatch-test`, and a fresh `--output-dir`.

Fresh no-NIC RTL, both constant headers and both XDC files match iteration
182 byte-for-byte. The October 1 immutable primary inputs and U250 XDC also
match, with the recorded implementation hierarchy rename. Their fresh
reports are `no-nic-rtl-collateral-comparison.json` and
`immutable-rocket-comparison.json`; no immutable fixture was edited.

The next smallest step is to connect the native AR response tracker to this
production NIC boundary and compare accepted requests and final R-beat
retirement with SFC's thirteen-bin reorder queue. R/B arbitration, remaining
widget bindings, host memory/driver stream lists, NIC runtime, other target
lowering profiles and partitioned QSFP remain gaps. Pre-existing user
ClockBridgeControl/UART registry edits are excluded from this checkpoint.
No FireSim manager verification step was started in this turn.

Fresh standalone replay of all thirteen archived handoffs and effective
platform configurations used compiler SHA256
`e0f22f09f1527fef09b9e8aecd60fd03ca2e528b7e6089ea4e9cb1ad5f1b38f1`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations; nonzero exit is
intentional. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle model-port annotations match; the other six retain exactly the prior
native-SRAM annotation differences. `matrix-regression-comparison.json`
checks unchanged inputs, effective platforms, statuses and structured
boundary results against iteration 182. Only NIC's later unsupported-feature
reason advances. `nic-prior-boundaries-comparison.json` confirms all eight
NIC decoder, queued write-route, dispatch and widget-write MLIR/annotation
artifacts are byte-identical to those already compared with SFC.

## Iteration 184: connect the production NIC AR response tracker

The native compiler now runs `addControlReadTracker` after NIC AR dispatch,
verifies the CIRCT FIRRTL operations, and emits
`post-fame-control-read-tracker.mlir` and complete annotations. The tracker
uses the twelve-region allocation: eight bank retirements, four explicit
unbound retirements, and error endpoint 12. Its 64 slots retain twelve-bit
transaction identity and four-bit route data. The NIC target still explicitly
fails at missing response arbitration/platform integration.

The exact SFC artifact compared is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlReadTrackerCompare.py` checks all 223 production wrapper connections,
165 copied ports, preservation of 116 prior module bodies, and recursive
transfer of all 1,792 annotation records. It matches 24 original `FPGATop`
response allocation connections and thirteen SFC router retirement/tag
expressions. The exporter runs the actual pass on the production AR boundary
and clones its dispatch and tracker operations, including assertions; both
helper bodies and the full transformed circuit match the compiler output.

Isolated Verilator comparison preserves the original SFC `ReorderQueue` body
and 46 original router expressions. It passes 100,352 cycles with 21,914
accepted reads, 28,271 stalls, 19,824 final retirements, 21,026 held beats,
6,516 alias stalls, 3,652 simultaneous accept/retirement cycles and 254 reset
cycles. All thirteen endpoints, full 64-slot storage, different upper tag
bits sharing a low-six-bit slot, out-of-order retirement and pending-request
reset are covered. Reset is clocked before checking initialized free slots;
two initial testbench reset failures remain as diagnostic logs. SFC and
CIRCT agreed at those failures; the scoreboard had run before reset took effect.

Normal SFC MMIO response `last` is optimized to one; differential execution
uses that specialization, while the native wrapper test exercises all eight
ready/valid/last combinations, including nonfinal normal responses. The error
endpoint retains burst-last behavior. SFC optimizes away unused route storage;
the native route payload is independently checked against a scoreboard.
Arbitration readiness is an explicit boundary. This comparison does not prove
R/B arbitration, widget completion, host memory arbitration, stream-driver ABI
or complete NIC runtime/platform behavior.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration184-u250-target-matrix/
```

`nic-control-read-tracker-production-verified/comparison.json` records hashes,
structured checks and behavioral results. After sourcing the FireSim
environment, run `test/Transforms/NICControlReadTrackerCompare.py` with
`--reads` and `--tracker` pointing to the production boundaries, `--golden`
to the archived NIC SV, `--export-tool` to `goldengate-control-read-tracker-test`,
and a fresh `--output-dir`.

`nic-prior-boundaries-comparison.json` freshly matches all ten earlier NIC
decoder, write-route, write-dispatch, widget-write and AR-dispatch MLIR/annotation
artifacts against iteration 183 byte-for-byte. Fresh no-NIC RTL, both constant
headers and both XDC files also match iteration 183. The October 1 immutable
primary inputs and U250 XDC match with the recorded implementation hierarchy
rename. No immutable fixture was changed, and no FireSim manager verification
step was started. Pre-existing user ClockBridgeControl/UART registry edits
remain outside this checkpoint.

Fresh standalone replay of all thirteen archived handoffs and effective
platform configurations used compiler SHA256
`26eda544eb3aa168e4e60a38c7d30da760d662fd588c76a18693793f1f0db81d`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations; its nonzero exit
reports those implementation gaps. All seven reached BridgeIO/channel
boundaries match SFC. Shuttle model-port annotations match; the other six
retain their previous native-SRAM annotation differences.
`matrix-regression-comparison.json` checks all thirteen inputs, effective
platforms, statuses and structured boundary results against iteration 183.
Only NIC's later unsupported-feature reason advances to response arbitration.

Focused CTest `goldengate-control-read-tracker` passes in 745.42 seconds:
16,443 collision/retirement/reset cycles across 1, 2, 3, 11, 12, 13, 31 and
63 regions plus empty bindings, all wrapper final-beat truth-table cases,
target transfer and 31 atomic rejection checks. `ctest.log` and
`control-read-tracker-ctest-detail.log` retain the result and detailed output.
The compiler and tracker test targets build successfully; Python syntax and
`git diff --check` also pass.

The next smallest step is to connect the thirteen-input R arbiter to this
tracker boundary and compare burst locking, priority, backpressure and final
retirement against the archived SFC `HellaPeekingArbiter`. Remaining widget
bindings, write responses, host memory/driver stream lists, NIC runtime,
wider/multiple TracerV profiles, FASED geometry and partition/QSFP remain gaps.

## Iteration 185: connect the production NIC control R arbiter

The native compiler now invokes `addControlReadArbiter` after the production
NIC AR tracker, verifies its CIRCT FIRRTL operations, and emits
`post-fame-control-read-arbiter.mlir` plus complete annotations. The helper
arbitrates twelve normal response sources plus error source 12, preserves
round-robin priority and burst locking, and connects final-beat retirement.
Eight allocated widget R channels are bound; four remaining widget R sources
stay explicit. Their tracker retirement inputs are now connected to their own
ready/valid/last and twelve-bit ID. Retained widget B fields remain at the
replacement boundary. NIC compilation still explicitly fails at missing
control write responses/platform integration.

The exact SFC artifact compared is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlReadArbiterCompare.py` verifies all 292 production wrapper
connections, 184 copied ports, preservation of 118 prior module bodies,
and recursive transfer of all 1,792 annotation records. It checks 32 SFC
`FPGATop` bank-allocation connections, all thirteen router R input bindings,
retirement/tag expressions, host clock/reset and master R output connections.
The production exporter runs the actual pass on the fresh tracker boundary
and clones the actual helper operations. Its full transformed circuit and
helper body match the compiler output.

Isolated Verilator comparison uses the entire original SFC
`HellaPeekingArbiter` logic. It passes 156,555 cycles: all 8,192 valid-source
masks at each of thirteen reachable cursors, 23,416 accepted beats, 129,773
stalls, 3,337 locked gaps, 1,666 nonfinal beats and 141 reset cycles. All
thirteen sources are exercised. Selected payloads, ungated readiness,
round-robin selection, error burst locks, final-beat retirement and reset
priority agree with SFC and an independent state scoreboard.

Normal SFC responses specialize `last=1`, `resp=0` and `user=0`; error data
is zero, response is DECERR and user is zero. The differential test preserves
those specializations. The native CTest also covers unspecialized payloads
and nonfinal normal responses. This comparison does not establish remaining
widget completion, write-response tracking, host memory/stream-driver ABI or
NIC runtime/platform parity.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration185-u250-target-matrix/
```

`nic-control-read-arbiter-production/comparison.json` records artifact hashes,
connection checks and behavioral results. Source the FireSim environment,
then run `test/Transforms/NICControlReadArbiterCompare.py` with `--tracker`
and `--arbiter` pointing to the fresh production boundaries, `--golden` to
the archived NIC SV, `--export-tool` to `goldengate-control-read-arbiter-test`,
and a fresh `--output-dir`.

Focused CTest `goldengate-control-read-arbiter` passes in 91.27 seconds:
121,756 selection/burst/reset cases across 1, 2, 3, 11, 12, 13, 31 and 63
regions, including the actual NIC allocation, wrapper wiring, annotation
transfer and 28 atomic rejections. The compiler and test targets build
successfully. `ctest.log` and `control-read-arbiter-ctest-detail.log` retain
the result and detailed output.

`nic-prior-boundaries-comparison.json` freshly matches all twelve NIC decoder,
write-route, write-dispatch, widget-write, AR-dispatch and AR-tracker
MLIR/annotation artifacts against iteration 184 byte-for-byte. Fresh no-NIC
RTL, both constant headers and both XDC files also match iteration 184.
The October 1 immutable primary inputs and U250 XDC match with the recorded
implementation hierarchy rename. No immutable fixture was changed, and no
FireSim manager verification step was started. Pre-existing user edits remain
outside this checkpoint.

Fresh replay of all thirteen archived handoffs and effective platform
configurations used compiler SHA256
`4c9eb51f2a11629adbae34e05d558a532498003bf51ae0d8ee7bd0ca28989601`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations. Its nonzero exit
reports these implementation gaps. All seven reached BridgeIO/channel
boundaries match SFC. Shuttle model-port annotations match; the other six
retain exactly their prior native-SRAM annotation differences.
`matrix-regression-comparison.json` checks all thirteen inputs, effective
platforms, statuses and structured boundary results against iteration 184.
Only NIC's later unsupported-feature reason advances to write responses.
Python syntax and `git diff --check` also pass.

The next smallest step is to connect the thirteen-input control B arbiter
and compare response selection, backpressure and retirement against the
archived SFC `RRArbiter`. Remaining widget bindings, write tracking, host
memory/stream lists, NIC runtime, wider/multiple TracerV profiles, FASED
geometry and partition/QSFP remain gaps.

## Iteration 186: connect the production NIC control B arbiter

The native compiler now invokes `addControlWriteArbiter` after production
NIC R arbitration, verifies its CIRCT FIRRTL operations, and emits
`post-fame-control-write-arbiter.mlir` plus complete annotations. Twelve
normal sources and error source 12 feed the round-robin arbiter. Eight
allocated widget B channels are connected; four remaining normal widget
sources stay explicit. Each of the thirteen write-retirement outputs uses
its own ready/valid acceptance and twelve-bit ID. NIC compilation still
explicitly fails at missing control write tracking/platform integration.

The exact SFC artifact compared is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlWriteArbiterCompare.py` checks all 304 production wrapper
connections, 206 copied ports, preservation of 120 prior module bodies,
and recursive transfer of all 1,792 annotation records. It checks 24 SFC
`FPGATop` bank-allocation connections, all thirteen router B input bindings,
AW retirement/tag expressions, host clock and master B output connections.
The production exporter runs the actual pass on the fresh R-arbiter
boundary and clones the actual helper operations. The transformed circuit
and cloned helper body match the compiler output.

Isolated Verilator comparison uses the entire original SFC `RRArbiter_0`
logic. It passes 312,274 cycles: all 8,192 valid-source masks at all sixteen
representable initial states with each output-ready value, followed by
continuous contention and seeded random traffic. This includes 156,132
accepted responses, 156,103 stalls, 39 empty cycles and 78,071 cycles with
multiple ready permissions. All thirteen sources are accepted. Selected
payloads, round-robin selection, ready permissions on invalid sources,
retirement and state hold/advance agree with SFC and an independent
scoreboard.

Both helpers leave `lastGrant` unreset. The test deposits equal initial
values into their actual registers, including out-of-range states 13, 14
and 15, without replacing arbitration expressions or sequential logic.
Normal SFC responses specialize `resp=0` and `user=0`; the error response
is DECERR with user zero. Differential execution preserves these
specializations; the native CTest separately covers general payloads.
This comparison does not establish outstanding-write tracking, remaining
widget completion, host memory/stream-driver ABI or NIC runtime/platform
parity.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration186-u250-target-matrix/
```

`nic-control-write-arbiter-production/comparison.json` records hashes,
connection checks and behavioral results. Source the FireSim environment,
then run `test/Transforms/NICControlWriteArbiterCompare.py` with
`--read-arbiter` and `--write-arbiter` pointing to the fresh production
boundaries, `--golden` to the archived NIC SV, `--export-tool` to
`goldengate-control-write-arbiter-test`, and a fresh `--output-dir`.

`nic-prior-boundaries-comparison.json` freshly matches all fourteen earlier
NIC decoder, AW/W route/dispatch, widget-write, AR dispatch/tracking and
R-arbitration FIRRTL/annotation artifacts against iteration 185 byte for
byte. Fresh no-NIC RTL, both constant headers and both XDC files also
match iteration 185. The October 1 immutable primary inputs and U250 XDC
match with the recorded implementation hierarchy rename. No immutable
fixture was changed, and no FireSim manager verification step was started.
Pre-existing user edits remain outside this checkpoint.

Focused CTest `goldengate-control-write-arbiter` passes in 300.02 seconds:
463,536 selection/acceptance/hold cases across 1, 2, 3, 11, 12, 13, 31 and
63 regions, including the actual NIC allocation, wrapper wiring, annotation
transfer and atomic rejections. The test exhausts all thirteen-source masks
and both ready values at all sixteen initial states with general payloads.
`ctest.log` and `control-write-arbiter-ctest-detail.log` retain the result.
The added geometry's rejection-summary counter was corrected after that
run; no behavioral code changed. A final rebuild and production export
reproduce the tested complete circuit and helper body, as recorded in
`production-export-final/comparison.json`. Python syntax and scoped
`git diff --check` also pass.

Fresh replay of all thirteen archived handoffs and effective platform
configurations used compiler SHA256
`06734af0c799f3499b8871f3ac19ed1837b1e158d10d0b0c48947d22cb59c5c6`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations. Its nonzero
exit reports these implementation gaps. All seven reached BridgeIO/channel
boundaries match SFC. Shuttle model-port annotations match; the other six
retain their prior native-SRAM annotation differences.
`matrix-regression-comparison.json` checks all thirteen inputs, effective
platforms, statuses and structured boundary results against iteration 185.
Only NIC's later unsupported-feature reason advances to write tracking.

The next smallest step is to connect outstanding AW tracking to these
thirteen B-retirement channels and compare slot allocation, tag aliasing,
backpressure, simultaneous enqueue/dequeue and reset against the archived
SFC `ReorderQueue`. Remaining widget bindings, host memory/stream lists,
NIC runtime, wider/multiple TracerV profiles, FASED geometry and
partition/QSFP remain gaps.

## Iteration 187: connect production NIC outstanding AW tracking

The native compiler now invokes `addControlWriteTracker` after production
NIC B arbitration, verifies its CIRCT FIRRTL operations, and emits
`post-fame-control-write-tracker.mlir` plus complete annotations. The
catalog's twelve banks and error endpoint 12 feed thirteen independent
retirement/tag inputs. AW acceptance captures the twelve-bit master ID and
four-bit decoded destination; tracker readiness closes the AW routing
capacity connection. NIC compilation still explicitly fails at remaining
control widget responses and platform integration.

The exact SFC artifact compared is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

SHA256: `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICControlWriteTrackerCompare.py` checks all 288 production wrapper
connections, 230 copied ports, preservation of 122 prior module bodies,
and recursive transfer of all 1,792 annotation records. It checks 24 SFC
`FPGATop` bank-allocation connections, all thirteen router B retirement
and ID expressions, clock/reset, and AW acceptance/capacity expressions.
The production exporter runs the actual pass on the fresh B-arbiter
boundary and clones the actual helper operations. Its complete transformed
circuit and helper body match the native compiler output.

Isolated Verilator comparison preserves the complete archived SFC
`ReorderQueue` body and passes 100,349 cycles: 39,174 accepts, 11,227 alias
stalls, 35,705 retirements, 248,058 held matches, 11,430 cycles with both
enqueue and retirement, and 255 reset cycles. Every retirement endpoint
is exercised. Directed cases fill all 64 slots, collide different full IDs
at each six-bit slot address, hold responses without retirement, and retire
old transactions while the new alias remains stalled at that edge. Readiness
and all full-ID matches agree with SFC and an independent scoreboard.

SFC removes unused route data; the native four-bit route storage is checked
against that scoreboard, including writes during reset. Reset initializes
free bits, while tag/data storage retains the Scala update behavior. The
native CTest separately checks mismatched retirement still freeing a slot,
pre-edge assertion predicates, reset-suppressed checks, and simultaneous
enqueue/dequeue priority. The differential test runs with assertions enabled
and supplies valid retirement traffic outside reset. It compares the AW
acceptance and B-retirement helper boundary, not an entire NIC simulator.
Remaining widget completion, host memory/stream-driver ABI and NIC runtime
or platform parity are not established by this comparison.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration187-u250-target-matrix/
```

`nic-control-write-tracker-production/comparison.json` records hashes,
connection checks and behavioral results. Source the FireSim environment,
then run `test/Transforms/NICControlWriteTrackerCompare.py` with
`--write-arbiter` and `--tracker` pointing to fresh production boundaries,
`--golden` to the archived NIC SV, `--export-tool` to
`goldengate-control-write-tracker-test`, and a fresh `--output-dir`.
The fresh tracker candidate SHA256 is
`cf37fea25b64e8407d4a51ff4bfbc88a04cb27d6298d45c71fe8af1e08e94708`.

`nic-prior-boundaries-comparison.json` freshly matches all sixteen earlier
NIC decoder, AW/W route/dispatch, widget-write, AR dispatch/tracking and
R/B-arbitration FIRRTL/annotation artifacts against iteration 186 byte for
byte. Fresh no-NIC RTL, both constant headers and both XDC files also
match iteration 186. The October 1 immutable primary inputs and U250 XDC
match with the recorded implementation hierarchy rename. No immutable
fixture was changed, and no FireSim manager verification step was started.
Pre-existing user edits remain outside this checkpoint.

Focused CTest `goldengate-control-write-tracker` passes in 305.03 seconds:
6,816 collision/retirement/reset cycles across 1, 2, 3, 11, 12, 13, 31 and
63 regions, including 1,827 cycles and thirteen assertions for the NIC's
actual allocation. Wrapper wiring, annotation transfer and fourteen atomic
rejections per geometry pass, as do invalid helper-count checks.
`ctest.log` and `control-write-tracker-ctest-detail.log` retain the result.
Python syntax and scoped `git diff --check` also pass.

Fresh replay of all thirteen archived handoffs and effective platform
configurations used compiler SHA256
`4616744897650b84e681b829dcc6e35ae616481d0a7dd52bce1022fbd18f5159`.
`replay/matrix.json` records one complete no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations. Its nonzero
exit reports these implementation gaps. All seven reached BridgeIO/channel
boundaries match SFC. Shuttle model-port annotations match; the other six
retain their prior native-SRAM annotation differences.
`matrix-regression-comparison.json` checks all thirteen inputs, effective
platforms, statuses and structured boundary results against iteration 186.
Only NIC's later unsupported-feature reason advances past write tracking.

The next smallest step is to attach and bind the catalog-selected
SimulationMaster MMIO bank at NIC slave 9, then compare its register
allocation, transport and response bindings with the archived SFC bank.
TSI, BlockDev and FASED responses, host memory/stream lists, NIC runtime,
wider/multiple TracerV profiles, FASED geometry and partition/QSFP remain gaps.


## Iteration 188: NIC SimulationMaster attachment, transport and slot-9 binding

The production compiler now lets NIC pass through `attachSimulationMasterBank`,
`mapSimulationMasterControl` and `bindSimulationMasterControl`. These passes
construct and connect CIRCT FIRRTL operations; the previous stop before bank
attachment is removed. NIC stops after the verified bound-master boundary,
with an explicit remaining TSI/BlockDev/FASED response and platform gap.
The progress message uses the decoder catalog instead of claiming fixed
slave 8: the twelve-region NIC catalog selects master slave 9.

The exact reference is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
`NICSimulationMasterCompare.py` compares the actual production attachment,
transport and bound FIRRTL/annotation boundaries. It matches addresses
576/580/584, all twenty surviving SFC control pins, thirty-two native scalar
bindings and aggregate AR dispatch, host clock/reset, unchanged prior module
bodies, and recursive transfer of all 1,792 annotations. It separately
replays the production mapping/binding passes and checks identical modules.
Its C++ exporter clones the actual bank and transport operations, then
`firtool` lowers those helpers for an isolated Verilator differential test.
The reference retains the archived `SimulationMaster` and `MCRFile` bodies.

The behavioral comparison passes 101,967 cycles, including 3,717 writes,
25,165 reads, 3,030 B stalls, 24,818 R stalls, 48 unused-index writes,
1,021 resets with pending traffic and 215 zero-strobe writes. Directed
traffic covers all four decoded indices, three AW/W arrival orders and all
sixteen strobe values, plus initialization delay and stalled reads. A further
100,000 randomized cycles cover response backpressure, IDs, address aliases,
register writes and reset. Five full native response constants match the
SFC-optimized interface. This proves this local bank/transport boundary,
not complete NIC simulation or platform/driver equivalence.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration188-u250-target-matrix/
```

`nic-simulation-master-production/comparison.json` records exact hashes and
results. Run `test/Transforms/NICSimulationMasterCompare.py` in the sourced
FireSim environment with `--tracker`, `--attached`, `--control` and `--bound`
pointing to the four corresponding production boundaries, `--golden` to the
NIC SV above, `--export-tool` to `goldengate-simulation-master-control-test`,
and a fresh `--output-dir`. The bound candidate SHA256 is
`9d931f34908ab2f24029b4d8f0a73ac91b1406bda309b0367dc77c0e292cddd2`.

Focused CTest `goldengate-simulation-master-control` passes 89 catalog
bindings and 185 atomic rejections, including explicit twelve-region slave-9
coverage and generic twelve-region coverage for all four supported widgets.
`goldengate-simulation-master` passes its 4,162 delay/sample/reset/write
cycles, allocation/split-equivalence checks and atomic rejections. Both
CTest cases finish in 1.95 seconds; detail is retained in
`simulation-master-ctest-detail.log`. Python syntax and scoped diff checks
also pass.

Eighteen earlier NIC FIRRTL/annotation artifacts match iteration 187 byte
for byte, as do no-NIC RTL, both constant headers and both XDC files.
`immutable-rocket-comparison.json` freshly matches the October 1 primary
FIRRTL/annotation handoff hashes and immutable U250 synthesis XDC; its
implementation XDC matches with the previously recorded hierarchy rename.
No immutable fixture was edited and no manager verification was started.
Pre-existing user changes remain outside this checkpoint.

`immutable-simulation-master-comparison.json` also compares the NIC reference
against the October 1 immutable U250 `design/FireSim-generated.sv`: both
`SimulationMaster` and `MCRFile` have identical ports and complete bodies
after comment removal. Thus the bank/transport reference remains the
recorded immutable compiler oracle; NIC changes its catalog allocation.

Fresh replay of all thirteen archived inputs and effective platform settings
used compiler SHA256
`dfb2915705825dedfac5ac9ec2a8dc53abf3d77c7fe538d0b10c00e163a39cb3`.
`replay/matrix.json` reports one completed no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations; its nonzero
exit explicitly retains these gaps. All seven reached BridgeIO/channel
boundaries match SFC. Shuttle model-port annotations match; the other six
retain their previously recorded native-SRAM differences.
`matrix-regression-comparison.json` matches all thirteen input hashes,
effective platforms, statuses and structured boundary results to iteration
187. Only the NIC diagnostic advances past the SimulationMaster binding.

The next smallest step is to carry NIC through the native TSI token/queue,
MMIO and catalog-selected control stages at slave 3, comparing them against
the same archived SFC target. BlockDev/FASED responses, host memory/stream
lists, NIC runtime, wider/multiple TracerV profiles, FASED geometry and
partition/QSFP support remain incomplete.

## Iteration 189: execute native TSI stages for the NIC catalog

The NIC compiler path now runs the five existing CIRCT TSI passes before
rejecting the remaining BlockDev/FASED and platform integration boundary.
They construct the scheduler, two sixteen-word queues, MMIO bank, control
transport and slave-3 bindings as FIRRTL operations. The catalog allocates
TSI at byte address 320, with nine registers through 352 in a 64-byte region.
The no-NIC path continues through the same passes.

The semantic oracle remains `MidasTransforms.scala`, `FAMETransform.scala`
and `generators/firechip/goldengateimplementations/src/main/scala/TSIBridge.scala`.
The latter gates target transfers on both host handshakes and remaining
tokens, resets queues only on host reset or an accepted target reset, and
gives a software start priority over token decrement. The comparison uses
the actual archived SFC artifact:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Its SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`test/Transforms/NICTSICompare.py` checks all five production wrappers,
recursive annotation retargeting (1,792 annotations), preservation of all
earlier modules, token predicates and payload connections, queue/MMIO
connections, the register catalog and all twenty SFC control pins.
The token check includes the HostPort predicate that excludes each
channel's own valid/ready signal. The test exporter in
`TSITokenEngineTest.cpp` clones the four actual native helper modules from
the bound production boundary; their bodies must match before lowering.

The isolated native assembly matches the complete SFC `TSIBridgeModule`,
`Queue_19`, `Queue_20` and `MCRFile_8` for 55,818 Verilator cycles.
Directed coverage includes both queues becoming full, simultaneous input
enqueue/dequeue, stalled and accepted target resets, host reset, sixteen
decoded MMIO indices, three AW/W arrival orders and all sixteen strobe
values. A further 50,000 randomized cycles exercise control response
backpressure, IDs, address aliases and target traffic. Observed coverage is
1,856 token fires, 1,367 B stalls, 12,271 R stalls, 2,193 full-input cycles,
1,344 full-output cycles, 1,603 stalled target-reset cycles and 47 accepted
target-reset cycles. Five SFC-optimized response constants also match.
This establishes the local TSI boundary; it does not establish complete NIC
runtime or platform/driver equivalence.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration189-u250-target-matrix/
```

`nic-tsi-production/comparison.json` records the production comparison.
The bound candidate SHA256 is
`c5360a5e8844c8f08dbf6c7099eabc50ad7654f7b45df567038a277b159f77c9`.
Run the comparison in the sourced FireSim environment with `--candidate-dir`
pointing to the NIC replay candidate, `--golden` to the SV above,
`--export-tool` to `goldengate-tsi-token-test` and a fresh `--output-dir`.
`immutable-tsi-comparison.json` also matches the entire TSI module against
the October 1 immutable U250 `design/FireSim-generated.sv` after dependency
type-name normalization; the two queues and MCRFile have identical ports
and complete bodies. No immutable fixture was edited.

Focused CTest cases `goldengate-tsi-token`, `goldengate-tsi-word-queues`,
`goldengate-tsi-mmio` and `goldengate-simulation-master-control` all pass
in 14.98 seconds. Python syntax and scoped diff checks pass. Twenty-six
earlier NIC FIRRTL/annotation files and all five no-NIC RTL/collateral files
match iteration 188 byte for byte. Fresh immutable handoff/XDC comparisons
also match, using the recorded implementation-XDC hierarchy rename.
No manager verification was started; pre-existing user edits are excluded.

Fresh replay of all thirteen archived inputs and effective platforms used
compiler SHA256
`08e1d1c8abdb9ea7557e9cdc536849bb017c86a369040fc75168e32f25361426`.
`replay/matrix.json` reports one completed no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations. The matrix's
nonzero exit retains these implementation gaps. All seven reached
BridgeIO/channel boundaries match SFC; Shuttle model-port annotations also
match, while the other six retain the recorded native-SRAM differences.
`matrix-regression-comparison.json` matches all thirteen input hashes,
effective platforms, compile statuses and structured boundary comparisons
to iteration 188. Only the NIC diagnostic advances past TSI binding.

The next smallest step is to carry NIC through native BlockDev token,
queue, MMIO and slave-0 control stages against the same archived SFC target.
FASED slave-1 responses, host memory/stream integration, NIC runtime,
wider/multiple TracerV profiles, FASED geometry and partition/QSFP support
remain incomplete.

## Iteration 190: execute native BlockDev stages for the NIC catalog

The NIC path now executes the eleven existing CIRCT BlockDev stages through
response scheduling before rejecting FASED/platform integration. These stages
construct the token engine, four functional queues, MMIO bank, control slave,
slave-0 binding, two latency pipes and response scheduler as verified FIRRTL
operations. The catalog assigns BlockDev byte address 0 in a 128-byte region,
with 26 register words through byte address 100. The no-NIC path uses the same
passes and retains identical emitted RTL/collateral.

The inspected semantic oracle remains `MidasTransforms.scala`,
`FAMETransform.scala` and
`generators/firechip/goldengateimplementations/src/main/scala/BlockDevBridgeModule.scala`.
The BlockDev constructor has one tracker, 64 beats per sector, 24-bit target
cycle/latency state, four functional queues of depths 10/32/32/4 and a
write-priority response scheduler. Target reset depends on both host
handshakes, even when queue/timing conditions block target firing; the cycle
counter resets only on host reset. The comparison uses the actual SFC artifact:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Its SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`test/Transforms/NICBlockDevCompare.py` verifies all eleven production stages,
preservation of every earlier module and recursive transfer of all 1,792
annotations, including the nine BlockDev channels. It checks the complete token
wrapper connections, MMIO register metadata, queue geometry, timing/control
connections and twenty SFC control pins; 25 additional assembly connections
tie the behavioral assembly to the production wrappers. The exporter in
`BlockDevTokenEngineTest.cpp` clones the nine actual helper modules from the
production response-scheduler boundary; their bodies must match before lowering.

The isolated native assembly matches the complete SFC `BlockDevBridgeModule`
and its actual queues, latency pipes and MCRFile for 40,799 Verilator cycles,
with assertions enabled. Directed coverage includes a 64-beat read, a complete
sector write, delayed software data/acknowledgement and queue-full target
reset. Another 40,000 randomized cycles exercise host/target reset, AW/W arrival,
MMIO address aliases, IDs, strobes and B/R backpressure. Observed coverage is
10,558 target fires, 65 responses (64 read beats and one write acknowledgement),
1,898 stalled target-reset cycles, 145 queue-reset cycles with target firing
blocked, 964 B stalls and 9,948 R stalls. Five SFC-optimized response constants
also match. This tests the local BlockDev boundary; complete NIC target runtime
and platform/driver equivalence remain unestablished.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration190-u250-target-matrix/
```

`nic-blockdev-final/comparison.json` records the production comparison and
candidate SHA256
`0d369f569d25310e4d39c0de379302861732f72d947decc5ce6107acef2bb6a0`.
Run the comparison in the sourced FireSim environment with `--candidate-dir`
pointing to the NIC replay candidate, `--golden` to the SV above, `--export-tool`
to `goldengate-blockdev-token-test` and a fresh `--output-dir`.
`immutable-blockdev-comparison.json` also compares the entire SFC BlockDev
widget and seven dependency instances with the October 1 immutable U250
`design/FireSim-generated.sv`: all ports and complete module bodies match.
Neither immutable fixture was edited. Fresh immutable Rocket handoff and
normalized XDC comparisons also match, with the recorded implementation-XDC
hierarchy rename.

All ten focused BlockDev CTest cases pass in 236.88 seconds. The refactored
Rocket/Print comparison still passes both constructor orders against the
immutable U250 SV. Python syntax and scoped diff checks pass. All 36 earlier
NIC boundary files and all five no-NIC RTL/collateral files match iteration
189 byte for byte. No manager verification was started; unrelated user changes
are excluded from this checkpoint.

Fresh replay of all thirteen archived inputs/effective platforms used compiler
SHA256 `b8750b1cbe49d659ebaacc0532897da727b2ae09c62002cd617c9980940086de`.
`replay/matrix.json` retains one completed no-NIC compile, six later lowering
failures and six unsupported partition/QSFP configurations; its nonzero exit
retains these gaps. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle model-port annotations match; the other six retain the recorded
native-SRAM differences. `matrix-regression-comparison.json` matches all
thirteen input hashes, effective platforms, compile statuses and structured
boundary comparisons to iteration 189. Only the NIC rejection advances past
BlockDev response scheduling.

The next smallest step is to carry NIC through the native FASED token and
slave-1 control stages against the same archived SFC target. Host memory/stream
integration, complete NIC runtime, wider/multiple TracerV profiles, FASED
geometry and partition/QSFP support remain incomplete.

## Iteration 191: execute native FASED token, counters and ingress for NIC

The NIC compiler path now constructs and verifies five existing CIRCT passes:
FASED token handling, host outstanding counters and AW/W/AR ingress queues.
Each stage emits production FIRRTL MLIR and retained annotations. NIC then
rejects the remaining ingress-credit, timing/control-response and platform
integration gap. This advances the actual compiler; the complete NIC target
still fails compilation and has no runtime-support claim.

The inspected Scala oracle is `MidasTransforms.scala`, `FAMETransform.scala`,
`midas/models/dram/FASEDMemoryTimingModel.scala`, `IngressUnit.scala` and the
saturating counter in `midas/widgets/Lib.scala`. Token firing requires both
host handshakes, ingress capacity, read/write egress readiness and settled
host memory during target reset. Ingress reset excludes its own capacity
condition; egress reset requires target firing. Host counters use host reset
and accepted host AXI transactions, independent of target reset. This catalog
has maxFlight 10, two four-bit counters and ingress depths AW=10, W=16, AR=4.

The actual SFC comparison artifact is:

```text
automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

Its SHA256 remains
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`test/Transforms/NICFASEDIngressCompare.py` checks all five production
boundaries, preservation of every earlier module, the complete transfer of
1,792 annotations, eleven memory channels and 46 payload routes. It checks
all 256 token/reset combinations and every production wrapper connection.
The six generated helpers match the no-NIC production helpers exactly.
The refactored Print comparators retain their original entry points; both
Print constructor orders still pass the ingress comparison.

The exporter in `FASEDTokenEngineTest.cpp` clones the six actual production
helpers from `post-fame-fased-ingress-ar.mlir`; exported bodies must match
before lowering. An isolated Verilator differential compares the three
queues with SFC `Queue_7`, `Queue_8`, `Queue_9`, and the two host counters with
SFC `SatUpDownCounter_1`. All 40,061 cycles match, including full/empty queues,
simultaneous push/pop, host reset and randomized payloads/backpressure.
Observed coverage is 57,384 accepted pushes, 52,918 pops, 3,090 full stalls,
6,920 empty stalls and 631 accepted pushes while reset is asserted. AW/AR
retain all 69 native payload bits and W retains 78; the comparison covers
the SFC-surviving 64/73/64 bits. The native unit tests exercise all fields.
This differential checks the queue/counter boundary; coupled host issue,
egress timing, MMIO and full NIC target execution remain outside it.

Evidence is under:

```text
sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration191-u250-target-matrix/
```

`nic-fased-final-comparison.json` records candidate SHA256
`fc0c248fa188f99ffbcd3abccee7b70ffb0b243307fd883be09d6b6541e9ff71`.
Run the comparator in the sourced FireSim environment with `--candidate-dir`
pointing to the NIC replay candidate, `--baseline` to the no-NIC production
`post-fame-fased-ingress-ar.mlir`, `--golden` to the SV above, `--export-tool`
to `goldengate-fased-token-test`, a fresh `--output-dir` and `--report-json`.
Five focused FASED CTest cases pass in 34.28 seconds; the token test also
passes after rebuilding the exporter. Python syntax and scoped diff checks
pass. All 72 earlier NIC MLIR/annotation files and five no-NIC RTL/collateral
files match iteration 190 byte for byte.

`immutable-fased-comparison.json` compares the complete FASED widget,
IngressModule, three queues and saturating-counter module with the October 1
immutable U250 `design/FireSim-generated.sv`: all six headers and bodies match.
Fresh immutable Rocket handoff and normalized XDC comparisons also match,
including the recorded implementation-XDC hierarchy rename. Both immutable
fixtures remain untouched. No manager verification was started.

Fresh replay of all thirteen archived inputs/effective platforms used compiler
SHA256 `78c432acc5aad4216eea4caad59947a1ddfc62d34b9a2b2f3760ba30d1510f51`.
`replay/matrix.json` retains one compiled no-NIC target, six later compiler
failures and six unsupported partition/QSFP configurations. Its nonzero exit
retains these gaps. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle model-port annotations match; the other six retain the recorded
native-SRAM differences. `matrix-regression-comparison.json` matches all
thirteen input hashes, effective platforms, compile statuses and structured
boundary comparisons to iteration 190. Only the NIC diagnostic advances.

The next smallest step is to carry NIC through native ingress credits and
transaction-order queues against the same SFC IngressModule, before host
issue, egress timing and slave-1 control binding. Complete NIC runtime,
wider/multiple TracerV profiles, FASED geometry and partition/QSFP support
remain incomplete.

## Iteration 192: execute native ingress credits and transaction ordering for NIC

The NIC compiler now executes and verifies `addFASEDIngressCredits` and
`addFASEDIngressOrder`, emitting both production MLIR boundaries and their
complete annotation archives. Its rejection moves to host request issue,
timing/control responses and platform integration. The shared credit and
ordering operations remain unchanged; this makes them executable on the
archived NIC input. Complete NIC compilation and runtime remain unsupported.

The Scala references inspected are `MidasTransforms.scala`, `FAMETransform.scala`,
`IngressUnit.scala`, `models/dram/Util.scala` and `widgets/Lib.scala`.
The credit pass implements the two four-bit saturating counters with maximum
ten, strict/relaxed decrement selection and completed-write detection. The
ordering pass implements `DualQueue` using two ten-entry one-bit memories,
bank selection and pointer/reset behavior.

The exact SFC artifact compared is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICFASEDCreditsOrderCompare.py` checks every production wrapper connection
(193 credit-stage and 196 ordering-stage connections), all 1,792 annotation
rows, all earlier module bodies, 8,192 credit states and 25,600 ordering
states. The generated helpers match the no-NIC helpers exactly. Both original
Print constructor orders pass the refactored comparison entry points.

The exporter clones eight actual production helpers and checks that their
bodies are preserved. An isolated Verilator differential cycles the two
credit/order helpers together against SFC `SatUpDownCounter_1`, `DualQueue`
and `Queue_10`. SFC completion/decrement expressions come from the recorded
`IngressModule`; completed writes feed its actual ordering queue. All 40,061
cycles match. Coverage includes 1,852 accepted dual enqueues, 4,277 asymmetric
dual attempts, 14,705 full stalls, 45 empty stalls, 21 accepted reset pushes,
6,802 saturation observations and both strict/relaxed policies. Accepted
queue events are boundary inputs here: full host request issue, ingress
payload queues, egress timing/MMIO and NIC execution are outside this test.
SFC's unused `DualQueue.next` lookahead is not exported.

Evidence is in the generated-src target's `iteration192-u250-target-matrix/`:
`nic-credit-order-comparison.json`, `nic-credit-order-differential/`,
`print-credit-regression.json`, `print-order-regression.json` and `ctest.log`.
The production order candidate SHA256 is
`86f0c34a7e9a6aa63c9d903fdb64f87e1333d91305f2ed310c8e0bbc30392fa4`.
Run the new comparator in the sourced environment with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the no-NIC production
`post-fame-fased-ingress-order.mlir`, `--golden` to the SFC SV above,
`--export-tool` to `goldengate-fased-token-test`, a fresh `--output-dir`
and `--report-json`. Native token/credits/order CTests pass in 16.81 seconds;
the original six-helper exporter also passes. Python syntax and scoped diff
checks pass. All 178 earlier NIC MLIR/annotation files and five no-NIC
RTL/collateral files match iteration 191 byte for byte.

`immutable-credits-order-comparison.json` checks complete headers/bodies of
`IngressModule`, `DualQueue`, `Queue_10` and `SatUpDownCounter_1` against the
October 1 immutable U250 `design/FireSim-generated.sv`: all four match.
Fresh immutable Rocket handoff and normalized XDC comparisons also match,
including the recorded implementation-XDC hierarchy rename. The immutable
fixtures remain untouched. No manager verification was started.

Fresh replay of all thirteen archived inputs/effective platforms used compiler
SHA256 `ba7599fc7e992029a1484be0959b179261b07b155f11e3f8fe90e94817ed5b04`.
`replay/matrix.json` retains one compiled no-NIC target, six later compiler
failures and six unsupported partition/QSFP configurations. Its nonzero exit
retains these gaps. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle model-port annotations match; the other six retain the recorded
native-SRAM differences. `matrix-regression-comparison.json` matches all
thirteen input hashes, effective platforms, compile statuses and structured
boundary comparisons to iteration 191. Only the NIC diagnostic advances.

The next smallest step is to carry NIC through native host request issue,
comparing strict/relaxed request gating and actual queue/order retirement
with the same SFC `IngressModule`. Egress timing, slave-1 control binding,
host memory/stream integration, wider/multiple TracerV profiles, FASED
geometry and partition/QSFP support remain incomplete.

## Iteration 193: execute NIC host request issue and accepted order retirement

The NIC compiler now executes and verifies `addFASEDIngressIssue`, emitting
`post-fame-fased-ingress-issue.mlir` and its complete annotation archive before
rejecting the remaining ingress assertions, timing/control responses and
platform composition. This connects native strict/relaxed AW/W/AR request
gates, qualified ingress reset, accepted queue/order retirement and host
outstanding feedback on the actual archived NIC input. Full NIC compilation
and runtime remain unsupported.

The Scala references inspected are `MidasTransforms.scala`, `FAMETransform.scala`
and `models/dram/IngressUnit.scala`. The recorded SFC oracle is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICFASEDIssueCompare.py` checks every production issue-wrapper connection,
complete recursive annotation transfers, retained module bodies, all 32,768
combinational cases, and exact equality with the shared no-NIC issue helper.
The original selected Print comparisons retain their defaults and pass both
constructor orders.

The production exporter clones nine actual helpers without rebuilding their
operations. An isolated Verilator differential cycles issue and transaction
ordering together against actual SFC `IngressModule` equations and its
write-data register update, coupled to recorded `DualQueue` and `Queue_10`.
It checks every native payload field for identity and all 21 surviving SFC
payload fields against their actual queue wires. Directed queue fill and
randomized direct/coupled operation cover host stalls, strict/relaxed policy,
read overlap, simultaneous AW/final-W acceptance, reset with accepted traffic,
and transaction-order retirement. Ingress credit/payload-queue events and host
status are boundary inputs; this does not establish complete ingress queues,
response/timing/MMIO behavior, NIC execution or platform parity.

Evidence is in the generated-src target's `iteration193-u250-target-matrix/`:
`nic-issue-comparison.json`, `nic-issue-differential/`,
`print-issue-regression.json`, `ctest.log`, `immutable-issue-comparison.json`,
`immutable-rocket-comparison.json`, `nic-prior-boundaries-comparison.json`,
`no-nic-rtl-collateral-comparison.json` and `replay/matrix.json`.
The immutable comparison matches complete headers/bodies of `IngressModule`,
`DualQueue` and `Queue_10` to the October 1 U250 fixture. Fresh immutable Rocket
handoff and normalized XDC comparisons match, including the recorded host
hierarchy rename. Five no-NIC RTL/collateral artifacts remain byte identical
to iteration 192. The immutable fixtures were not modified. No manager
verification was started.

The issue comparison matches all 205 wrapper connections, all 1,792 annotation
rows, 170 retained module bodies and 32,768 combinational cases, including
160 AW/final-W collisions. All 80,025 differential cycles match, with 436
AW/final-W collisions, 15 resets with accepted traffic, 26,174 host stalls,
1,070 overlapping read acceptances, 22,471 full order-queue stalls and 10,208
accepted order retirements. Both strict and relaxed policies have over 39,000
cycles. The production issue candidate SHA256 is
`d23e11cf2f4a2c8e82226c71d2d809783ec9076c4066be03955430edab6ac391`.
Native issue/token CTests pass in 31.04 seconds. Both Print regressions pass;
all 182 prior NIC MLIR/annotation files match iteration 192 byte for byte.

Run the new comparator in the sourced environment with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the no-NIC production
`post-fame-fased-ingress-issue.mlir`, `--golden` to the SFC SV above,
`--export-tool` to `goldengate-fased-token-test`, a fresh `--output-dir`
and `--report-json`.

Fresh replay of all thirteen archived inputs/effective platforms used compiler
SHA256 `72626ffa0c52f781548bb6baf06141d25bebf22472bfb00114c48c2b1bcb6e82`.
`replay/matrix.json` retains one compiled no-NIC target, six later compiler
failures and six unsupported partition/QSFP configurations. Its nonzero exit
retains these gaps. All seven reached BridgeIO/channel boundaries match SFC.
Shuttle model-port annotations match; the other six retain the recorded native
SRAM differences. `matrix-regression-comparison.json` matches all thirteen input
hashes, effective platforms, compile statuses and structured boundary checks to
iteration 192. Only the NIC diagnostic advances.

The next smallest step is to execute NIC ingress deadlock assertions against
the same SFC oracle, then port the remaining response/timing and slave-1
control binding. Wider/multiple TracerV profiles, FASED geometry, complete NIC
host memory/stream composition and partition/QSFP support remain incomplete.

## Iteration 194: execute NIC ingress enqueue deadlock assertions

The actual NIC compiler now executes and verifies `addFASEDIngressDeadlock`
after host issue, emitting `post-fame-fased-ingress-deadlock.mlir` and its
complete annotation archive. The pass connects the actual AW/W queue enqueue
valid/ready signals, strict/relaxed policy, credit emptiness and order validity
through six internal policy ports. Assertions use the host clock and qualified
ingress reset. The compiler rejects subsequent response buffering,
timing/control responses and platform composition; NIC runtime remains unsupported.

The Scala references inspected are `MidasTransforms.scala`, `FAMETransform.scala`
and `models/dram/IngressUnit.scala`. The selected SFC artifact is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
`NICFASEDDeadlockCompare.py` matches all 1,185 production wrapper connections,
all 1,792 annotation rows, 165 unaffected module bodies and the shared no-NIC
assertion helper. It retains the 32,768 issue cases and 160 AW/final-W collision
cases. The selected Print comparator retains its defaults; both constructor
orders pass after accepting equivalent inline and automatic-local mux lowering.

The native deadlock test exports an unchanged clone of the production helper
and a second clone exposing its actual predicate/enable operands. The exporter
does not reconstruct policy or enqueue expressions. An isolated Verilator
differential compares the lowered operands with recursively extracted actual
SFC assertion expression cones across all 512 input combinations: 32 W
failures, 32 AW failures and 256 reset-disabled cases match. The unchanged
production assertions are clocked in safe cases and two separate deliberate
failure runs require their exact recorded W/AW diagnostics. This establishes
boundary assertion behavior; it does not establish complete queue scheduling,
response/timing/MMIO behavior or NIC simulator execution.

Evidence is in the generated-src target's `iteration194-u250-target-matrix/`:
`nic-deadlock-comparison.json`, `nic-deadlock-differential/`,
`print-deadlock-regression.json`, `ctest.log`,
`immutable-deadlock-comparison.json`, `immutable-rocket-comparison.json`,
`nic-prior-boundaries-comparison.json`,
`no-nic-rtl-collateral-comparison.json` and `replay/matrix.json`.
The production candidate SHA256 is
`4fa862e1799d3daef35062e0ae4299ec316c6312910a9dbeeee668f662fefeff`.
Native deadlock CTest passes its 512 behavior cases and 16 atomic rejection
cases for each baseline/selected constructor. All 184 prior NIC MLIR/annotation
files and five no-NIC RTL/collateral files match iteration 193 byte for byte.

The complete `IngressModule` header/body in the NIC archive matches the
October 1 immutable U250 `design/FireSim-generated.sv`, including both
assertion guards, predicates and messages. Candidate predicate/enable semantics
also match that immutable oracle across 512 cases. Fresh comparisons of the
immutable primary Rocket handoff and normalized U250 XDC match, including
the recorded implementation-XDC hierarchy rename. Neither immutable fixture
was modified. No manager verification was started.

Run the new comparator in the sourced environment with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the no-NIC production
`post-fame-fased-ingress-deadlock.mlir`, `--golden` to the SFC SV above,
`--export-tool` to `goldengate-fased-ingress-deadlock-test`, a fresh
`--output-dir` and `--report-json`.

Fresh replay uses the same thirteen archived inputs and their effective
platform configurations. Compiler SHA256 is
`9dc55aa553eb79cc937db1de1e05b090fad30d3c6a368bbdc9427ab51c6df9cd`.
Full matrix results and the comparison to iteration 193 are recorded in
`replay/matrix.json` and `matrix-regression-comparison.json`.
The replay retains one compiled no-NIC target, six later compiler failures
and six unsupported partition/QSFP platforms; its nonzero exit preserves
these implementation gaps. All seven reached BridgeIO/channel boundaries
match SFC. Shuttle model-port annotations match; the other six retain the
recorded native-SRAM differences. All thirteen input hashes, effective
platforms, compile statuses and structured boundary comparisons match
iteration 193. Only the NIC unsupported-feature diagnostic advances.

The next smallest step is to execute NIC read-response buffering against
the SFC `ReadEgress`/`MultiQueue` boundary, checking qualified reset, per-ID
occupancy, synchronous prefetch and data/last behavior. Timing/control
responses, slave-1 binding, wider/multiple TracerV profiles, generalized FASED
geometry, complete NIC host memory/stream composition and partition/QSFP
support remain incomplete.

### Iteration 195: execute native NIC read-response storage

The compiler now executes `addFASEDReadBuffer` on the actual NIC replay and
emits verified `post-fame-fased-read-buffer.mlir` and its complete annotation
archive before rejecting the remaining NIC response scheduling, timing/control
and platform integration. This is the existing CIRCT sixteen-ID, eight-beat
non-ROB queue, driven by actual host response data/last/ID, host clock and
qualified egress reset. The no-ROB SFC `ReadEgress` host ready remains constant
one; its internal queue full signal is not substituted for that ready.

Changes are confined to `tools/goldengate.cpp`, the production-helper export
mode in `test/Transforms/FASEDReadBufferTest.cpp`, this ledger, and the new
`test/Transforms/NICFASEDReadBufferCompare.py`. The exporter clones the actual
production FIRRTL helper, including its RAM and control operations, verbatim.
The comparison lowers that clone through firtool and compiles it alongside
the complete actual recorded SFC `MultiQueue` RTL, renaming only its module
symbol. It does not reconstruct the oracle's sequential state equations.

The exact matrix golden is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(relative to the outer repository). The complete `ReadEgress` and `MultiQueue`
headers/bodies also match the immutable
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
Geometry, selector, pointer/full/reset and enqueue/dequeue connections match
the Scala `EgressUnit.scala` / `widgets/Lib.scala` oracle.

Fresh evidence is under the no-NIC U250 generated directory's
`iteration195-u250-target-matrix/`. `nic-read-buffer-comparison.json` reports
40,322 matching clock cycles and 21,910 defined payload comparisons, including
74 full-queue observations, 142 reset-time writes, 25,034 ID switches,
5,444 simultaneous enqueue/dequeue cases and 26 pointer wraps. All 48 pointer
and occupancy registers, the delayed ID, handshake and synchronous prefetch
state are compared on every edge. Every payload RAM location is initialized
before payload comparisons; unspecified initial memory and same-address
read/write payloads are excluded. The run observes 1,310 read/write collisions;
none produces a valid undefined payload requiring a skipped comparison.
This is bounded differential evidence for storage, not NIC runtime parity.

The actual NIC wrapper retains 173 earlier modules, all 199 connections and
1,792 annotation records with exact recursive target transfer. All 186 prior
NIC FIRRTL/annotation artifacts match iteration 194; only the two read-buffer
artifacts are added. The working no-NIC Rocket's final RTL, both driver headers
and both XDC files remain identical. Fresh immutable primary Rocket handoff
hashes and normalized U250 XDC comparisons also match, including the recorded
implementation-XDC hierarchy rename. Neither immutable fixture was modified.
The focused CTest passes 40,000 state/edge samples and 18 atomic rejection
cases for each baseline and selected Print profile. No manager verification
was started.

Run the new comparator in the sourced environment with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the no-NIC production
`post-fame-fased-read-buffer.mlir`, `--golden` to the SFC SV above,
`--export-tool` to `goldengate-fased-read-buffer-test`, a fresh absolute
`--output-dir` and `--report-json`.

Fresh thirteen-target replay uses compiler SHA256
`05138ec0c31ea704bbf8042255d8ce9209059856e8bd963f4bb2c868cadaeb2a`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen input hashes, effective platforms, compile statuses
and structured boundary comparisons match iteration 194. All seven reached
BridgeIO/channel boundaries match SFC; Shuttle model-port annotations match,
while the other six retain the recorded native-SRAM differences. The replay's
nonzero exit preserves these gaps. Only the NIC diagnostic advances.

The next smallest step is to execute native NIC read-response request
scheduling against `ReadEgress`, checking held ID/valid, request priority,
targetFire, token readiness and final-beat retirement together with this RAM.

### Iteration 196: execute native NIC read-response scheduling

The actual NIC compiler now executes `addFASEDReadScheduler` and emits verified
`post-fame-fased-read-scheduler.mlir` and its complete annotation archive.
The existing CIRCT scheduler consumes the qualified host reset and targetFire,
holds the unreset four-bit request ID, prioritizes a new request over final-beat
retirement, addresses synchronous read prefetch and drives read token validity.
The explicit failure moves to unimplemented NIC write responses, timing/control
responses and platform integration; NIC compile/runtime support is incomplete.

Changes are confined to `tools/goldengate.cpp`, the production export mode in
`test/Transforms/FASEDReadSchedulerTest.cpp`, the new
`test/Transforms/NICFASEDReadSchedulerCompare.py`, and this ledger. The exporter
clones both actual production FIRRTL helpers, including the queue RAM and every
scheduler/pointer expression. Its small test top uses the two checked production
wrappers' connections, including constant host ready for the recorded non-ROB
profile. The differential compiles those helpers beside complete actual SFC
`ReadEgress` and `MultiQueue` RTL, renaming only module symbols.

The exact golden is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(relative to the outer repository), SHA256
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The complete `ReadEgress`/`MultiQueue` headers and bodies also match immutable
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
The actual FASED reset, request, targetFire and response-ready connections match
the SFC timing-model boundary. The Scala `EgressUnit.scala` oracle's request
priority and unreset ID behavior are preserved through FIRRTL operations.

Evidence is under the no-NIC generated directory's
`iteration196-u250-target-matrix/`. `nic-read-scheduler-comparison.json` reports
40,338 matching cycles, 22,063 defined active-response payload checks, 9,620
request starts, 3,627 retirements, 1,818 chained requests, 75 reset-time request
captures, 23,897 stalls, 315 full-queue observations, 149 reset-time writes,
6,016 ID changes and 25 pointer wraps. All 48 queue pointer/occupancy registers,
prefetch ID, active-request state, defined held ID and scheduler/queue coupling
are compared. RAM is fully initialized before payload checks. Same-address
read/write payloads and power-on ID/memory are excluded.

The initial differential exposed an undefined-RUW `last` consumption at cycle
759, followed by different active-valid state at cycle 760. Both RAMs specify
undefined read/write behavior. The corrected test stalls request retirement
after each collision until a defined read, while retaining all control checks;
it observes 1,559 collisions. This is a test-domain restriction, not a compiler
semantic fix or a claim about undefined last-bit consumption.

The actual scheduler wrapper preserves 175 prior modules, checks all 195
connections and transfers all 1,792 annotations recursively. The underlying
queue composition retains its separately checked 199 connections. All 188
prior NIC FIRRTL/annotation artifacts match iteration 195; only the two
scheduler artifacts are added. The no-NIC Rocket's final RTL, both headers
and both XDC outputs remain byte-identical. Fresh primary immutable handoff
hashes and normalized U250 XDC comparisons match, including the documented
implementation hierarchy rename. Neither immutable fixture was modified.
Focused CTest passes 32,768 scheduler edge cases, 32 wrapper mapping cases,
eight target transfers and 20 atomic rejections for each of the baseline and
selected Print profiles. No manager verification was started.

Run the comparator in the sourced environment with `--candidate-dir` pointing
to the NIC replay, `--baseline` to the no-NIC production scheduler MLIR,
`--golden` and `--immutable` to the two SFC SV files above, `--export-tool` to
`goldengate-fased-read-scheduler-test`, a fresh absolute `--output-dir`, and
`--report-json`.

The fresh thirteen-target replay uses compiler SHA256
`683eb4a40040abdaa4f80f795a9a3ebc49885b779ffa19348ddfcdae697ff3b6`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen input hashes, effective platforms, compile statuses
and structured boundary comparisons match iteration 195. All seven reached
BridgeIO/channel boundaries match SFC; Shuttle model-port annotations match,
while the other six retain the documented native-SRAM differences. Only the
NIC diagnostic advances. The matrix's nonzero exit preserves these gaps.
Older mutable replay MLIR from iterations 190 and 191 is compressed with
decompressed SHA256 verification and manifests; no immutable golden or current
candidate is compressed or modified by that disk-space maintenance.

The next smallest step is to execute native NIC per-ID write acknowledgement
storage/scheduling against the actual SFC `WriteEgress` RTL, preserving qualified
reset, request priority and the combined read/write token-readiness boundary.
Timing/control responses, slave-1 binding, wider/multiple TracerV profiles,
generalized FASED geometry, complete NIC host memory/stream composition and
partition/QSFP support remain incomplete.

### Iteration 197: execute native NIC write acknowledgment storage/scheduling

The compiler now runs `addFASEDWriteEgress` for the archived NIC constructor.
It emits the actual native per-ID acknowledgment operations, connects host B
acceptance and write-token readiness, and retains the read scheduler's shared
read/write readiness boundary. The NIC rejection moves to the still-unported
response release, timing/control and platform composition boundary. This is
partial NIC support, not a successful full NIC compile or runtime claim.

The Scala oracle is `EgressUnit.scala`'s `WriteEgress`: one-bit counters for
all sixteen IDs, old-counter sampling on request start or host-only retry,
retry selection of the held ID, simultaneous same-ID arrival/retirement
cancellation, new-request priority over retirement, and qualified egress reset.
The held ID captures during reset while valid, haveAck and counters reset.
`FASEDMemoryTimingModel.scala` qualifies reset with `targetFire`; the model
response release clock is gated separately, as prescribed by the FAME path
in `MidasTransforms.scala` and `FAMETransform.scala`.

Evidence is in the mutable generated directory
`iteration197-u250-target-matrix/`. `nic-write-egress-comparison.json` compares
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-write-egress.mlir`
and its `-all.json` against the actual SFC module and model connections in:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The two complete SFC `WriteEgress` modules match. The new comparator exports
the production helper verbatim using `goldengate-fased-write-egress-test`,
lowers it through firtool and executes it against the complete SFC sequential
RTL in Verilator. Only the SFC module symbol is renamed. All nineteen state
registers and acknowledgment outputs match after initial reset/ID capture:
40,130 cycles, 10,153 starts, 3,951 retirements, 1,992 chained requests,
78 reset-time captures, 7,781 host-only retries, 7,782 counter wraps,
776 same-ID cancellations, 1,221 different-ID simultaneous updates,
3,118 old-counter samples and 23,374 stalls. All sixteen IDs participate in
arrivals, cancellation and wrapping. Incoming response metadata is discarded;
native response user/resp remain zero. There is no uninitialized RAM or
undefined-RUW restriction in this write differential.

The wrapper preserves 177 prior modules, checks all 196 connections, verifies
the exact new port types against the no-NIC production boundary and transfers
all 1,792 annotations recursively. All 190 earlier NIC FIRRTL/annotation
artifacts match iteration 196; only two write-egress artifacts are added.
The no-NIC Rocket's final RTL, both headers and both XDC files remain
byte-identical. Fresh primary immutable handoff hashes and normalized U250
XDC comparisons also match, including the recorded implementation hierarchy
rename. Neither immutable fixture was modified. Focused CTest passes 42,768
write-egress transitions, 64 wrapper mapping cases, five target transfers and
21 atomic rejections for each baseline/selected Print profile. Manager gates
remain owned by the external harness and were not started here.

Reproduce the comparator in the sourced environment with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the no-NIC production write-egress
MLIR, `--golden` and `--immutable` to the SFC SV files above, `--export-tool`
to `goldengate-fased-write-egress-test`, a fresh absolute `--output-dir` and
`--report-json`. The preflight report uses iteration 196's no-NIC helper;
the final NIC report uses the fresh iteration 197 production boundary.

The fresh thirteen-target replay uses compiler SHA256
`489ef10be5313f4ec7c6c1fe507df0fae7cca9f0d2f357b5b6e618252c8c6322`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. Input hashes, effective platform configurations, compile statuses
and structured boundary comparisons match iteration 196 for all thirteen
targets. Only the NIC diagnostic advances. Rejections remain implementation
gaps, not passing comparisons. All seven reached bridge/channel boundaries
match SFC; Shuttle model-port annotations match, while the other six retain
the documented native-SRAM differences. Older mutable iteration 192 MLIR
was gzip-compressed with decompressed SHA256 verification and a manifest,
recovering 35,214,476,370 bytes; current candidates and goldens are retained.

The next smallest step is to connect native NIC response release and compare
the targetFire-qualified read/write occupancy and payload forwarding with
the actual SFC `AXI4Releaser` RTL. Timing/control responses, slave-1 binding,
generalized FASED geometry, wider/multiple TracerV profiles, complete NIC
host memory/stream composition and partition/QSFP integration remain incomplete.

### Iteration 198: connect native NIC target response release

The compiler now runs `addFASEDResponseReleaser` for the archived NIC model.
It emits the native read/write occupancy queues and connects target responses,
egress requests and readiness. State and model reset are qualified by
`targetFire`; read occupancy retires only on an accepted last beat, and pipe
replacement retains occupancy. Response payloads remain combinational even
while invalid. The explicit NIC rejection now follows this boundary and still
reports unported timing/control responses and platform integration. Full NIC
compilation and runtime support remain incomplete.

The executable Scala oracle is `Util.scala`'s `AXI4Releaser`, its one-entry
`Queue(..., pipe = true)` instances, and the model clock/reset connections in
`FASEDMemoryTimingModel.scala`. `MidasTransforms.scala` and
`FAMETransform.scala` retain the target-token/host-clock distinction.
`TimingModel.scala` connects the releaser to target responses and completion
metadata; timing completion remains the next boundary.

Evidence is in the mutable generated directory
`iteration198-u250-target-matrix/`. `nic-response-releaser-comparison.json`
compares the actual NIC production
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-response-releaser.mlir`
and `-all.json` with these exact SFC artifacts:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The complete SFC `AXI4Releaser`, `Queue_12` and `Queue_13` modules match the
immutable reference. The comparator also checks `LatencyPipe` and
`FASEDMemoryTimingModel` clock/reset and egress binding. It clones the actual
native helper verbatim through `goldengate-fased-response-releaser-test`, lowers
it with firtool, and executes it against the complete SFC releaser and both
queue modules in Verilator. Only the releaser module symbol is renamed; the
oracle model clock is qualified by `targetFire`, as in the SFC model.

All fourteen SFC-visible outputs and both occupancy registers match before
and after clock edges: 40,081 cycles, 1,968 read replacements, 7,905 non-last
beats, 11,847 blocked read requests, 168 occupied stalled resets, 9,492
combinational acceptances without target fire, 2,034 empty-queue pushes and
18,880 invalid response-forwarding cases. All sixteen read and write request
IDs participate. SFC prunes user/resp and unused length ports; native user/resp
forwarding is additionally checked against the Scala contract, not claimed
as a comparison against nonexistent SFC pins. There is no uninitialized RAM
or undefined-RUW restriction in this occupancy differential.

The production wrapper preserves 179 modules, checks all 205 connections,
matches the exact added port types to the fresh no-NIC boundary and transfers
all 1,792 annotations recursively. The 192 earlier NIC FIRRTL/annotation
artifacts match iteration 197 byte-for-byte; only two response-releaser
artifacts are added. The no-NIC final RTL, both headers and both XDC files
remain identical to iteration 197. Fresh comparisons also match both primary
immutable handoff hashes and normalized immutable U250 XDC with the recorded
implementation hierarchy rename. Neither immutable fixture was modified.

Focused CTest passes 18,192 target-response transitions, 64 wrapper mapping
cases, seven target transfers and 22 atomic rejections for each baseline and
selected Print profile. Reproduce the new comparator with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the fresh no-NIC response-releaser
MLIR, `--golden` and `--immutable` to the SV paths above, `--export-tool` to
`goldengate-fased-response-releaser-test`, a fresh absolute `--output-dir` and
`--report-json`. Preflight uses iteration 197's no-NIC helper; the final report
executes iteration 198's NIC production helper.

The fresh thirteen-target replay uses compiler SHA256
`ea92796e66e2c63c410bea8f168f57459c5f439834def6be30dd72b51aa4784c`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen archived input hashes, effective platform semantics,
compile statuses and structured boundary comparisons match iteration 197;
only the NIC diagnostic advances. All seven reached bridge/channel boundaries
match SFC, while six documented native SRAM model-port differences remain.
These partial matches do not establish full target support or semantic parity.
Manager verification steps were not started here. Older mutable iteration 193
MLIR was compressed with decompressed SHA256 verification and a manifest,
recovering 35,258,192,318 bytes; current candidates and goldens are retained.

The next smallest step is to connect native NIC `FASEDTimingCycle` and compare
its target-qualified 64-bit cycle count and latency-derived release deadlines
with the actual SFC `LatencyPipe` behavior. Timing completion queues, control
responses/slave-1 binding, generalized FASED geometry, multiple/wider TracerV
profiles, complete NIC host memory/stream composition and partition/QSFP
integration remain incomplete.

### Iteration 199: connect native NIC model cycle and release deadlines

The compiler now invokes `addFASEDTimingCycle` for the archived NIC model.
It emits a native UInt64 counter with targetFire-qualified increment/reset,
UInt32 runtime read/write latency inputs and modulo-2^64 release deadlines
with the one-cycle egress offset. The explicit NIC rejection follows this
boundary and still reports unported timing completion, control responses and
platform integration. Full NIC compilation/runtime support remains incomplete.

The executable Scala oracle is `TimingModel.scala` 100–102 and
`LatencyBandwidthPipe.scala` 67/81, with the gated model clock and reset in
`FASEDMemoryTimingModel.scala` 355–404. `MidasTransforms.scala` and
`FAMETransform.scala` retain the model-clock/host-token distinction. The
timer is implemented through existing CIRCT FIRRTL operations; this change
connects that pass in the actual NIC compiler path.

Evidence is in the mutable generated directory
`iteration199-u250-target-matrix/`. `nic-timing-cycle-comparison.json` compares
the actual NIC production
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-timing-cycle.mlir`
and `-all.json` with these exact SFC artifacts:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The original SFC `LatencyPipe` counter declaration, update and deadline
arithmetic cone match the immutable reference, as does `AbstractClockGate`.
The comparator pins `FASEDMemoryTimingModel` clock/reset/fire binding. It
clones the actual production native helper verbatim through
`goldengate-fased-timing-cycle-test`, lowers it with firtool and runs Verilator
against the original SFC counter/arithmetic cone. Independent latency/reset
inputs and observed output pins replace only the surrounding model boundary.
Identical state seeds are deposited with both clocks low to reach UInt64
wraparound in finite time; the following register updates remain unmodified.

All three outputs match before and after targetFire-qualified edges over
41,153 cycles: 20,587 stalls, 439 stalled resets, 430 enabled resets, 20,136
increments, 36 counter wraps, 353 zero-latency and 352 max-latency cases,
45 deadline underflows, 128 deadline overflows and 20,298 latency changes
while stalled. The production wrapper preserves 181 modules, checks all
196 connections, matches the five added port types to the fresh no-NIC
boundary and transfers all 1,792 annotations recursively.

**Clock-gate limitation:** the initial preflight executed the archived
`AbstractClockGate` shim literally. Its posedge-sampled enable register
produced a transient model edge when CE deasserted between host pulses,
giving SFC cycle 1 versus native cycle 0. The failing test/log are retained in
`timing-cycle-preflight/` and described in
`clock-gate-preflight-diagnostic.json`. The final differential compares the
Scala targetFire-qualified model-edge contract, using `clock & targetFire`;
it does not claim delta-cycle equivalence of that shim or complete clock-gate
parity. `recorded-clock-gate.sv` retains the original inspected module.

The 194 earlier NIC FIRRTL/annotation artifacts match iteration 198
byte-for-byte; only two timing-cycle artifacts are added. No-NIC final RTL,
both headers and both XDC files remain identical to iteration 198. Fresh
comparisons match both primary immutable handoff hashes and normalized
immutable U250 XDC with the recorded implementation hierarchy rename.
Neither immutable fixture was modified.

Focused CTest passes 21,152 deadline/counter transitions, 16 wrapper mapping
cases, three target transfers and 19 atomic rejections for each baseline and
selected Print profile. Reproduce the new comparator with `--candidate-dir`
pointing to the NIC replay, `--baseline` to the fresh no-NIC timing-cycle MLIR,
`--golden` and `--immutable` to the SV paths above, `--export-tool` to
`goldengate-fased-timing-cycle-test`, a fresh absolute `--output-dir` and
`--report-json`. Preflight v2 uses iteration 198's no-NIC helper; the final
report executes iteration 199's NIC production helper.

The fresh thirteen-target replay uses compiler SHA256
`500e71751235d5ef12534c32fbf1fd172c5da9dc32134c82b15bd54918448bdc`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen archived input hashes, effective platform semantics,
compile statuses and structured boundary comparisons match iteration 198;
only the NIC diagnostic advances. All seven reached bridge/channel boundaries
match SFC, while six documented native SRAM model-port differences remain.
These partial matches do not establish full target support or semantic parity.
Manager verification steps were not started here. Older mutable iteration 194
MLIR was compressed with decompressed SHA256 verification and a manifest,
recovering 35,301,928,500 bytes; current candidates and goldens are retained.

The next smallest step is to connect native NIC `FASEDReadLatency` and compare
its ten-entry completion queue, release deadlines, target reset and retirement
against the actual SFC read-pipe RTL. Write latency/AW completion, control
responses/slave-1 binding, generalized FASED geometry, multiple/wider TracerV
profiles, complete NIC host memory/stream composition, the recorded clock-gate
shim behavior and partition/QSFP integration remain incomplete.

### Iteration 200: connect native NIC read completion queue

The actual NIC compiler path now invokes `addFASEDReadLatency`, verifies the
native FIRRTL operations and emits `post-fame-fased-read-latency.mlir` and
`-all.json`. The wrapper consumes the read release deadline and next-read
metadata ports, binds accepted AR requests to the shared ten-entry queue and
connects due completions to the existing response releaser. State and RAM
writes advance on targetFire-qualified host edges. The explicit NIC rejection
now identifies write timing completion, control responses and platform
integration; the NIC target is still unsupported as a complete compilation.

The executable Scala oracle is `LatencyBandwidthPipe.scala` 77–88,
`TimingModel.scala` 90–102 and `FASEDMemoryTimingModel.scala` 355–404, with the
model/host clock distinction in `MidasTransforms.scala` and `FAMETransform.scala`.
The implementation uses the existing CIRCT queue, memory, comparison, reset
and connection operations, rather than implementing Scala syntax.

Evidence is in the mutable generated directory
`iteration200-u250-target-matrix/`. `nic-read-latency-comparison.json` compares
the production NIC
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-read-latency.mlir`
and `-all.json` against these exact SFC artifacts:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The original SFC `Queue_16` module, release/retirement equations from
`LatencyPipe` and recorded clock-gate module match the immutable reference
after whitespace/comment normalization. `NICFASEDReadLatencyCompare.py`
exports the actual production helper verbatim through the focused C++ test,
lowers it with firtool and executes it against the original queue RTL and
release equations in Verilator. The queue's ten 64-bit deadline and 4-bit ID
entries are compared with the native packed RAM; only previously written
locations are observed, so uninitialized RAM is not treated as defined data.
The native 8-bit read length, optimized out of SFC RTL, is checked separately
against an independent metadata scoreboard. Native overflow assertion failure
is tested with a separate deliberately invalid enqueue; the SFC admission and
reset predicate are pinned from the recorded parent RTL.

All queue handshake/ID outputs, pointers and full state match before and after
every edge over 40,197 cycles: 129 empty flows, 652 full-queue pops, 20,131
stalls, 201 stalled resets, 42 enabled reset edges that still write RAM,
798 pointer wraps, 1,619 simultaneous enqueue/dequeue cases, 22,501 blocked
head cases, 1,990 equal deadlines and 21,291 maximal deadlines. Every initialized
RAM entry is compared on every edge, including reset and stall. The production
wrapper retains 183 prior modules, checks all 200 connections and exact copied
port types, and transfers all 1,792 annotations recursively. Consumed ports
retain their inner module identity. The helper matches the fresh no-NIC
production helper exactly.

The earlier clock-gate limitation remains: this differential executes the
Scala targetFire-qualified model-edge contract with `clock & targetFire`.
It pins the SFC host clock/reset/targetFire binding and retains the archived
clock-gate module in `nic-read-latency-differential/recorded-clock-gate.sv`,
but does not execute or claim delta-cycle parity for the posedge enable-register
shim diagnosed in iteration 199. Full NIC integration and complete clock-gate
semantics remain unproved.

Focused CTest passes 30,093 transitions, 16 wrapper mapping cases, five target
transfers and 19 atomic rejections for each baseline/selected Print profile.
The new comparator accepts `--candidate-dir` for the NIC replay, `--baseline`
for fresh no-NIC read-latency MLIR, `--golden` and `--immutable` for the paths
above, `--export-tool` for `goldengate-fased-read-latency-test`, a fresh absolute
`--output-dir`, and `--report-json`. A preflight using iteration 199's no-NIC
helper passed the same positive RTL cases; the final report executes this
iteration's NIC helper and the negative overflow assertion test.

All 196 earlier NIC FIRRTL/annotation artifacts match iteration 199
byte-for-byte; only two read-latency artifacts are added. The no-NIC final RTL,
both headers and both XDC files are identical to iteration 199. Both primary
immutable handoff hashes and normalized immutable U250 XDC match, including
the recorded implementation hierarchy rename. Immutable fixtures are unchanged.

The fresh thirteen-target replay uses compiler SHA256
`6faf16137b374ecd03eb4a17e7bf9244d4b69e80466f6fd22fb3ca0e9f8b181d`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen archived input hashes, effective platform semantics,
statuses and structured boundary comparisons match iteration 199; only the NIC
unsupported diagnostic advances. Reached bridge/channel boundaries match SFC;
the six previously documented native SRAM model-port differences remain.
Partial boundary comparisons do not imply full target support. No manager
verification step was started here. Older mutable iteration 195 MLIR was
compressed with decompressed SHA256 verification and a manifest, recovering
35,345,759,234 bytes without modifying current candidates or any golden.

The next smallest step is to connect native NIC `FASEDWriteLatency` and compare
its completion input, ten-entry write deadline queue and retirement with SFC.
AW/W pairing, control responses/slave-1 binding, generalized FASED geometry,
multiple/wider TracerV profiles, complete NIC host memory/stream composition,
the recorded clock-gate shim behavior and partition/QSFP integration remain
incomplete.

### Iteration 201: connect native NIC write deadline queue

The actual NIC compiler path now invokes `addFASEDWriteLatency`, verifies the
native FIRRTL operations and emits `post-fame-fased-write-latency.mlir` and
`-all.json`. The wrapper consumes the write release deadline and next-write
ports and connects the shared ten-entry deadline queue to the response
releaser. Its completion input still requires `newWReq` and the oldest accepted
AW ID. The NIC rejection is retained immediately before `addFASEDTimingAWQueue`
and now identifies AW/W completion pairing, control responses and platform
integration. This is a partial compiler boundary, not complete NIC support.

The executable Scala oracle is `LatencyBandwidthPipe.scala` 61–74 and
`TimingModel.scala` 234–254, with the host/model clock distinction in
`FASEDMemoryTimingModel.scala`, `MidasTransforms.scala` and `FAMETransform.scala`.
The implementation executes the existing CIRCT memory, register, comparison,
assertion and connection operations; no Scala syntax is translated.

Evidence is in the mutable generated directory
`iteration201-u250-target-matrix/`. `nic-write-latency-comparison.json` compares
the fresh production NIC
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-write-latency.mlir`
and `-all.json` against these exact SFC artifacts:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The original SFC `Queue_15`, the write release/retirement equations from
`LatencyPipe` and recorded clock-gate module match the immutable reference
after whitespace/comment normalization. `NICFASEDWriteLatencyCompare.py`
exports the production helper verbatim through `FASEDWriteLatencyTest.cpp`,
lowers it with firtool and executes it against the original queue and release
equations in Verilator. It pins enqueue to the parent RTL's `newWReq` and
oldest AW ID contract; the AW/W counters themselves remain outside this test.
All initialized 64-bit deadlines and 4-bit IDs in both RAM implementations
are compared on every edge. Uninitialized RAM is not treated as defined data.
A separate invalid enqueue executes and rejects the native overflow assertion;
the SFC parent assertion's admission and reset predicate are pinned as well.

All queue handshake/ID outputs, pointers and full state match before and after
every edge over 40,197 cycles: 145 empty flows, 651 full-queue pops, 20,074
stalls, 195 stalled resets, 43 enabled reset edges that still write RAM,
792 pointer wraps, 1,647 simultaneous enqueue/dequeue cases, 22,505 blocked
head cases, 1,944 equal deadlines and 20,038 maximal deadlines. The production
wrapper retains 185 prior modules, checks all 198 connections and exact copied
port types, and recursively transfers all 1,792 annotations. Consumed ports
retain their inner module identity. The queue helper matches the fresh no-NIC
production helper exactly.

The clock-gate limitation from iteration 199 remains: this differential
executes the Scala targetFire-qualified model-edge contract with
`clock & targetFire`. It pins SFC host clock/reset/targetFire bindings and
retains the original gate module in
`nic-write-latency-differential/recorded-clock-gate.sv`, but does not execute
or claim delta-cycle parity for the recorded posedge enable-register shim.
Full NIC integration and complete clock-gate semantics remain unproved.

Focused CTest passes 30,093 transitions, 16 wrapper mapping cases, five target
transfers and 21 atomic rejections for each baseline/selected Print profile.
The comparator accepts `--candidate-dir` for the NIC replay, `--baseline` for
fresh no-NIC write-latency MLIR, `--golden` and `--immutable` for the paths above,
`--export-tool` for `goldengate-fased-write-latency-test`, a fresh absolute
`--output-dir`, and `--report-json`. An initial preflight uses iteration 200's
no-NIC helper; the final report executes this iteration's NIC helper.

All 198 earlier NIC FIRRTL/annotation artifacts match iteration 200
byte-for-byte; only two write-latency artifacts are added. The no-NIC final RTL,
both headers and both XDC files are identical to iteration 200. Both primary
immutable handoff hashes and normalized immutable U250 XDC match, including
the recorded implementation hierarchy rename. Immutable fixtures are unchanged.

The fresh thirteen-target replay uses compiler SHA256
`4477ca2cdf7e2e6a77659d54744466714818a6f00421be27e0d235f2521d16a6`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen archived input hashes, effective platform semantics,
statuses and structured boundary comparisons match iteration 200; only the NIC
unsupported diagnostic advances. Reached bridge/channel boundaries match SFC;
the six previously documented native SRAM model-port differences remain.
Partial boundary comparisons do not imply full target support. All thirteen
stderr logs were inspected. No manager verification step was started here.
Older mutable iteration 196 MLIR was compressed with decompressed SHA256
verification and a manifest, recovering 35,389,660,849 bytes without modifying
current candidates or any golden.

The next smallest step is to connect native NIC `FASEDTimingAWQueue` and compare
its accepted AW metadata, flow-through head ID, reset and retirement with SFC
`Queue_14`. Then the pending AW/W counters can supply the completion pulse.
Control responses/slave-1 binding, generalized FASED geometry, multiple/wider
TracerV profiles, complete NIC host memory/stream composition, the recorded
clock-gate shim behavior and partition/QSFP integration remain incomplete.

## Iteration 202: NIC accepted AW metadata queue

The production compiler now runs `addFASEDTimingAWQueue` for the NIC target,
verifies its CIRCT FIRRTL operations and emits the queue boundary and annotations
before rejecting the unported pending AW/W counters. The ten-entry flow queue
captures `AW.fire` and provides its oldest 4-bit ID to write completion. Capacity
remains an observation; it does not alter AW acceptance. The external completion
pulse remains explicit until the pending counters are connected. This extends
the actual compiler path; it does not claim a complete NIC compile.

Evidence is under the standard Rocket generated-source directory:

```text
iteration202-u250-target-matrix/
  build.log
  compiler.sha256
  ctest.log
  ctest-details.log
  nic-timing-aw-queue-comparison.json
  nic-timing-aw-queue-differential/
  nic-prior-boundaries-comparison.json
  rocket-output-regression-comparison.json
  immutable-rocket-comparison.json
  replay/matrix.json
  matrix-regression-comparison.json
```

The exact new boundary is
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-timing-aw-queue.mlir`,
SHA256 `920814ab0e3ac6ee82170ef2390bb26abb1e84409232ea0f1cbbfbcf1486ad71`.
Its `-all.json` carries all 1,792 annotations with recursive target transfer.
The wrapper retains 187 prior modules and all copied port types, and checks all
199 aggregate/scalar connections. The production helper equals the fresh
no-NIC helper exactly: three qualified-reset registers and a 10-by-4-bit RAM.

The SFC oracle is `Queue_14` in
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The queue and recorded clock-gate module match their counterparts in the
immutable U250 build-tree `design/FireSim-generated.sv` after comment/whitespace
normalization. The `LatencyPipe` bindings pin accepted AW ID, enqueue on AW.fire,
dequeue on `newWReq`, and oldest ID into the write-latency queue. The Scala oracle
is `models/dram/TimingModel.scala:234-254`.

`NICFASEDTimingAWQueueCompare.py` exports the actual production helper verbatim
through `FASEDTimingAWQueueTest.cpp`, lowers it with firtool, and executes it
against the original SFC queue RTL in Verilator. Outputs, pointer/full state and
all initialized RAM entries match before and after every edge over 40,197
cycles: 919 empty flows, 273 full-queue pops, 19,923 stalls, 192 stalled resets,
85 reset edges that still write RAM, 1,549 wraps and 3,946 simultaneous
enqueue/dequeue cases. All sixteen IDs are exercised. A separate invalid
enqueue executes and rejects the native overflow assertion with the original
SFC message. RAM writes remain independent of reset; unknown initial RAM data
is not used as defined reference data.

The differential executes targetFire-qualified model edges with
`clock & targetFire`, as in the Scala gated-clock contract. It pins the SFC
clock/reset/targetFire bindings and retains the original gate module in
`nic-timing-aw-queue-differential/recorded-clock-gate.sv`. It does not execute
or claim delta-cycle parity for the recorded posedge enable-register shim.
AW/W counter behavior and full NIC integration remain outside this test.

Focused CTest passes 30,092 transitions, 64 wrapper mapping cases, five target
transfers and 22 atomic rejections for each baseline/selected Print profile.
Reproduce the differential with `--candidate-dir` for the fresh NIC replay,
`--baseline` for fresh no-NIC AW queue MLIR, `--golden` and `--immutable` for the
paths above, `--export-tool` for `goldengate-fased-timing-aw-queue-test`, a fresh
absolute `--output-dir`, and `--report-json`. The preflight uses iteration 201's
no-NIC helper; the final report executes the fresh iteration 202 NIC helper.

All 200 earlier NIC MLIR/annotation artifacts match iteration 201 byte-for-byte;
only two AW queue artifacts are added. The no-NIC final RTL, both headers and
both XDC files are identical to iteration 201. Both immutable primary handoff
hashes and normalized immutable U250 XDC match, including the recorded
implementation hierarchy rename. Immutable fixtures are unchanged.

Older mutable iteration 197 MLIR was compressed with decompressed SHA256
verification and a manifest: 554 files, recovering 35,433,638,146 bytes.
Current candidates and completed SFC goldens were not modified by compression.

The fresh thirteen-target replay uses compiler SHA256
`8a5ae99e2458fcda5a87562c35dd9200a144cd184571c768b92463184e0e5df3`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC target, six later compiler failures and six unsupported partition/QSFP
platforms. All thirteen archived input hashes, effective platform semantics,
statuses and structured boundary comparisons match iteration 201; only the NIC
unsupported diagnostic advances. Reached bridge/channel boundaries match SFC;
the six documented native SRAM model-port differences remain. Partial matches
do not imply full target support. All thirteen stderr logs were inspected.
No FireSim manager verification step was started here.

The next smallest step is to connect native NIC `FASEDWritePairing` and compare
the pending AW/W counts, last-W handshake, target B retirement and completion
pulse with the SFC `LatencyPipe` counter cone. Control responses/slave-1 binding,
generalized FASED geometry, multiple/wider TracerV profiles, complete NIC host
memory/stream composition, recorded clock-gate shim behavior and partition/QSFP
integration remain incomplete.

## Iteration 203: NIC pending AW/W counters and completion pairing

The production compiler now runs `addFASEDWritePairing` on the NIC path after
the accepted-AW metadata queue, verifies the FIRRTL operations and emits the
pairing boundary and annotations. Two four-bit saturating counters advance and
reset only on targetFire. The pre-edge comparison of pending AW/W counts drives
the existing queue's completion pulse. Accepted AW and last-W handshakes remain
combinational; completion is not gated by full, reset or targetFire. The compiler
still rejects NIC before target B retirement and runtime admission are bound.
This is an additional native compiler boundary, not a complete NIC compile.

Evidence is in the standard Rocket generated-source directory:

```text
iteration203-u250-target-matrix/
  build.log
  compiler.sha256
  ctest.log
  ctest-details.log
  write-pairing-preflight.json
  write-pairing-preflight/
  nic-write-pairing-comparison.json
  nic-write-pairing-differential/
```

The exact new boundary is
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-write-pairing.mlir`,
SHA256 `3b8b3bfb42cdbe28351f7945e24ec409b0c579dbd7d00e03c4a6b9d1a7e92643`.
It retains 189 prior modules, all copied port types and all 1,792 annotations
with recursive target transfer. All 200 aggregate/scalar wrapper connections
match, including accepted AW, accepted last W, the external B-fire/runtime-max
ports, pending diagnostics and the completion pulse. The production helper
equals the fresh no-NIC helper exactly.

The exact SFC reference is `SatUpDownCounter_1` and the pending-counter/newWReq
cone in `LatencyPipe` in
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The counters, original AW/W/B handshake equations, pairing equations and
recorded clock-gate module match their counterparts in the immutable U250
build-tree `design/FireSim-generated.sv` after comment/whitespace normalization.
The Scala oracle is `models/dram/TimingModel.scala:108-114,226-239` and
`widgets/Lib.scala:71-86`; the FAME scheduling order and qualified channel
updates are preserved from `MidasTransforms.scala` and `FAMETransform.scala`.

`FASEDWritePairingTest.cpp` exports the actual production helper verbatim;
`NICFASEDWritePairingCompare.py` lowers that clone with firtool and executes it
against the original SFC counter instances and original handshake/pairing
equations in Verilator. Outputs and counter registers match before and after
every edge over 40,101 cycles: 20,050 stalls, 162 stalled resets, 195 target
resets, 2,340 AW-ahead pairs, 669 W-ahead pairs, 1,278 simultaneous pairs,
814 full-saturation cases, 3,198 empty retirements, 1,666 simultaneous inc/dec
cases and 5,112 observations above a lowered runtime maximum. All sixteen
runtime limits and all sixteen states of each counter are exercised. Lowering
the maximum does not clamp existing state; simultaneous inc/dec holds state.

The test executes targetFire-qualified model edges with `clock & targetFire`.
It pins the SFC host clock/reset/targetFire bindings and retains the original
gate in `nic-write-pairing-differential/recorded-clock-gate.sv`. It does not
execute or claim delta-cycle parity for the recorded posedge enable-register
shim. Full-model admission, MMIO maximum binding and observed target B-fire
attachment remain outside this boundary. The raw handshake cone is executed;
the native wrapper handshake mapping is also checked exhaustively by CTest.

Focused CTest passes 151,072 pending-counter/pairing transitions, 256 wrapper
mapping cases, five target transfers and 23 atomic rejections for each baseline
and selected Print profile. Reproduce the differential with `--candidate-dir`
for the fresh NIC replay, `--baseline` for fresh no-NIC pairing MLIR, `--golden`
and `--immutable` for the paths above, `--export-tool` for
`goldengate-fased-write-pairing-test`, a fresh absolute `--output-dir`, and
`--report-json`. The preflight uses iteration 202's no-NIC boundary; the final
report executes the fresh iteration 203 NIC helper.

`nic-prior-boundaries-comparison.json` confirms all 202 earlier NIC MLIR and
annotation artifacts are byte-identical to iteration 202; only the two pairing
artifacts are added. `rocket-output-regression-comparison.json` confirms the
no-NIC final SV, both headers and both XDC files are unchanged.
`immutable-rocket-comparison.json` confirms both immutable primary handoff
hashes and normalized immutable U250 XDC match, including the recorded
implementation hierarchy rename. Immutable fixtures remain untouched.

The fresh thirteen-target replay uses compiler SHA256
`6b0f3c91a3539e62c5c84b4a132ecf4776dfa8fe4e98b3feaa0a2a1f93302050`.
`replay/matrix.json` and `matrix-regression-comparison.json` preserve one
compiled no-NIC target, six later compiler failures and six unsupported
partition/QSFP platforms. All archived input hashes, effective platform
semantics, statuses and structured boundary comparisons match iteration 202;
only the NIC unsupported diagnostic advances. Eight annotation boundaries
match, six have the documented native SRAM model-port differences and twelve
are not reached. All thirteen complete stderr logs were inspected. Partial
boundary matches are not complete target support. No FireSim manager gate
was started by this implementation turn.

Mutable iteration 199 replay MLIR was compressed with decompressed SHA256
verification and a manifest: 556 files, recovering 35,521,786,722 bytes.
Current candidates and completed SFC goldens were not compressed or modified.

The next smallest step is to bind NIC `FASEDWriteRetirement`, observing the
response releaser's target `B.valid && B.ready` through the recorded sim
hierarchy and feeding both pending counters. Compare that feedback and the
consumed input/annotation transfer against the SFC `LatencyPipe` retirement
cone before connecting runtime maximum/admission. Control responses/slave-1
binding, generalized FASED geometry, multiple/wider TracerV profiles, complete
NIC host memory/stream composition, recorded clock-gate shim behavior and
partition/QSFP integration remain incomplete.

## Iteration 204: NIC target B retirement feedback

The production compiler now runs `bindFASEDWriteRetirement` on the NIC path,
verifies the resulting FIRRTL and emits its MLIR/annotation boundary before
rejecting the remaining NIC write-admission/control/platform integration.
The native pass observes response-releaser `B.ready && B.valid`, propagates it
through six recorded sim wrappers and closes the pending AW/W counter feedback.
The consumed external `fased_target_b_fire` input is removed. Retirement is
combinational and independent of reset/targetFire; the existing counters qualify
state updates and reset on targetFire. Host egress acceptance remains separate.

Evidence is in the standard Rocket generated-source directory:

```text
iteration204-u250-target-matrix/
  build.log
  compiler.sha256
  ctest.log
  ctest-details.log
  write-retirement-preflight.json
  write-retirement-preflight/
  write-pairing-regression.json
  write-pairing-regression/
  nic-write-retirement-comparison.json
  nic-write-retirement-differential/
```

The fresh NIC boundary is
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-write-retirement.mlir`,
SHA256 `6ba4dfbb19dbd699beac301703213c05a5089141b170022021c15d86e3562a79`.
The comparison retains all 191 prior module identities and 185 unchanged module
bodies, checks 1,204 prior/observation drivers and 194 wrapper connections,
preserves copied port types/directions, and transfers all 1,792 annotation
records recursively. The consumed target remains on the connected inner input;
copied targets move to the new wrapper. The pending-counter helper is unchanged
and equals the fresh no-NIC helper exactly.

The exact SFC artifact is the `LatencyPipe`, `AXI4Releaser`,
`SatUpDownCounter_1` and `FASEDMemoryTimingModel` cone in
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Both pending-counter decrement equations use target B-ready/B-valid. The
releaser drives target B-valid from the current-write queue, while host egress
B-valid uses next-write acceptance. These bindings, the counter/pairing cone
and recorded clock gate match the immutable U250 build-tree
`design/FireSim-generated.sv` after comment/whitespace normalization. The Scala
oracle remains `models/dram/TimingModel.scala:108-114`, with scheduling checked
against `passes/MidasTransforms.scala` and `passes/fame/FAMETransform.scala`.

`FASEDWriteRetirementTest.cpp` checks the actual production six-hop observation
and closed feedback, then exports the cloned B AND operation, original helper
connections and verbatim pending-counter helper. Accepted AW/last-W, target B
fields, model clock/reset/fire and runtime maximum are probe inputs. The six
wrapper hops are structurally checked; the isolated RTL executes the resulting
handshake/counter cone rather than the entire response queue or wrapper chain.
`NICFASEDWriteRetirementCompare.py` lowers the probe with firtool and executes
it against the original SFC counter instances and handshake/pairing equations.

The fresh NIC RTL differential passes 40,101 cycles with outputs and counter
registers checked before and after every edge. Coverage includes 5,066 accepted
B responses, 5,028 stalled B handshakes, 9,889 valid/backpressure cases, 10,114
ready-without-valid cases, 20,050 target stalls, 162 stalled resets, 195 qualified
resets, 1,666 simultaneous inc/dec cases and 3,198 empty retirements. All sixteen
counter states and runtime limits and all sixteen reset/fire/B-ready/B-valid
combinations are exercised. The preflight executes the iteration 203 no-NIC
boundary; the final report executes the fresh iteration 204 NIC boundary.
The unchanged pairing differential also passes with the shared test driver.

Model edges use `clock & targetFire`; the recorded gate module and host bindings
are pinned, but its enable-register delta-cycle behavior is explicitly excluded.
Response queue generation, runtime maximum MMIO attachment and complete host
integration remain outside this test. Focused CTest passes preservation of
existing users/attributes/targets, six observation hops and 20 atomic rejection
cases for each baseline and selected Print profile.

`nic-prior-boundaries-comparison.json` confirms all 204 earlier NIC MLIR and
annotation artifacts are byte-identical to iteration 203; only two retirement
artifacts are added. `rocket-output-regression-comparison.json` confirms the
no-NIC final SV, both headers and both XDC files are unchanged.
`immutable-rocket-comparison.json` confirms the immutable primary FIRRTL and
annotation hashes and normalized immutable U250 XDC, with the recorded
implementation hierarchy rename. Immutable fixtures were only read.

The fresh thirteen-target replay uses compiler SHA256
`31dbe7bb67facee604090ba1cce3d3a327f95984f2d1e6f3dbefd265d771ad52`.
`replay/matrix.json` and `matrix-regression-comparison.json` retain one compiled
no-NIC Rocket, six compiler failures and six unsupported partition/QSFP
platforms. All archived input hashes, effective platform configurations and
structured boundary records match iteration 203; only the NIC unsupported
feature advances to write admission. Eight partial annotation boundaries match,
six retain the documented SRAM model-port differences, and twelve are not
reached. All thirteen complete stderr logs were inspected. These are partial
boundary results rather than complete target support. No FireSim manager gate
was started by this implementation turn.

Older mutable iteration 200 and 201 replay MLIR was gzip-compressed with
verified decompressed SHA256 and manifests: 557/558 files, recovering
35,565,947,908/35,610,168,742 bytes respectively. Current candidate outputs and
completed SFC goldens were not compressed or modified.

The next smallest step is to bind NIC `FASEDWriteAdmission`, connecting target
AW/W readiness to the pending counter full flags and comparing the consumed
inputs/annotation transfer against SFC `TimingModel`/`LatencyPipe`. Runtime
maximum MMIO, read admission, control responses/slave-1 binding, generalized
FASED geometry, multiple/wider TracerV profiles, full NIC memory/stream host
composition, recorded clock-gate shim behavior and partition/QSFP integration
remain incomplete.

## Iteration 205: NIC independent AW/W admission

The native compiler now runs `bindFASEDWriteAdmission` on the NIC path,
verifies the resulting FIRRTL and emits its MLIR/annotation boundary before
rejecting the remaining NIC read-admission/request-limit/control/platform work.
AW and W ready are independent combinational inversions of their pending-counter
full flags, with no reset/fire/host-ready mask. Each driven ready feeds the inner
request handshake and remains observable at the new boundary. Only those two
bundle flips change; request valid/payload and reverse AR readiness pass through.
The existing target B retirement and pending-counter bindings are preserved.

Evidence is in the standard Rocket generated-source directory:

```text
iteration205-u250-target-matrix/
  build.log
  compiler.sha256
  ctest.log
  ctest-details.log
  write-admission-preflight.json
  write-admission-preflight/
  write-pairing-regression.json
  write-pairing-regression/
  export-rejections/report.json
  nic-write-admission-comparison.json
  nic-write-admission-differential/
```

The fresh NIC boundary is
`replay/alveo_u250_firesim_rocket_singlecore_nic/candidate/post-fame-fased-write-admission.mlir`,
SHA256 `15f63bb7445686313b3cddfac0e7b1330be02a6bd03b37fb3b4f8289536b4e8b`.
The comparison retains all 192 prior module definitions unchanged, compares
201 new-wrapper connections, and recursively transfers all 1,792 annotation
records. The complete request type matches the fresh no-NIC admission boundary;
the native exporter checks every field/flip and all copied port identities/types.
The pending-counter helper is unchanged and equals the fresh no-NIC helper.

The exact SFC artifact is the `LatencyPipe`, `IdentityModule`,
`SatUpDownCounter_1` and `FASEDMemoryTimingModel` cone in
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The original AW/W-ready assignments invert the respective full flag. The
accepted tNasti handshake sees that readiness through `nastiReqIden` and its
identity module. The counter/pairing, admission assignments, identity aliases
and model clock/reset bindings match the immutable U250 build-tree
`design/FireSim-generated.sv` after comment/whitespace normalization. The Scala
oracle is `models/dram/TimingModel.scala:108-114,226-231`; scheduling was checked
against `passes/MidasTransforms.scala` and `passes/fame/FAMETransform.scala`.

`FASEDWriteAdmissionTest.cpp` validates the actual production ready drivers and
full-flag sources, then exports cloned NotPrimOps and observed-ready connections.
The retirement exporter independently validates six production observation hops
and clones the B handshake, helper bindings and verbatim pending-counter helper.
`NICFASEDWriteAdmissionCompare.py` lowers both probes with firtool and closes the
full flags back to accepted AW/last-W. Candidate and SFC use separate readiness
feedback loops; the SFC executes its original ready assignments, counter bodies,
instances and handshake/pairing equations. Full production wrapper wiring and
annotation transfer are compared separately from the isolated RTL cone.

The fresh NIC differential passes 40,101 cycles, checking outputs and registers
before and after every edge. All sixteen counter states/runtime limits and all
sixteen reset/fire/B-ready/B-valid contexts are exercised. Coverage includes
7,879 blocked AW requests, 3,177 blocked W requests, 2,496 zero-limit cycles,
2,169 simultaneous inc/dec cases, 1,382 empty retirements, 13,446 cycles with a
limit below an existing count, 20,050 target stalls, 162 stalled resets and 195
qualified resets. There are no accepted increments into full counters in the
closed loop. B coverage includes 5,066 qualified acceptances and 5,028 stalled
handshakes. The preflight uses iteration 204 no-NIC IR; the final report uses
fresh iteration 205 NIC IR. The existing pairing differential also passes.

Focused CTest passes payload/AR preservation, target transfer and 16 atomic
rejections for each baseline and selected Print profile. Two valid production-IR
mutations are rejected before export: crossed AW/W full flags and an observed
ready differing from accepted ready. Model edges use `clock & targetFire`; the
recorded gate module is pinned but its enable-register delta-cycle behavior is
excluded. Request/response queue generation and runtime maximum MMIO remain
outside this differential. This is a partial NIC compiler boundary.

`nic-prior-boundaries-comparison.json` confirms all 206 earlier NIC MLIR/JSON
artifacts are byte-identical to iteration 204; only two admission artifacts are
added. `rocket-output-regression-comparison.json` confirms unchanged final
no-NIC SV, both headers and both XDC files. `immutable-rocket-comparison.json`
confirms the immutable primary `.sfc.fir`/`.anno.json` hashes and normalized U250
XDC, with the recorded implementation hierarchy rename. Immutable fixtures were
only read.

The fresh thirteen-target replay uses compiler SHA256
`bb752b89a0aff27de4701b1634f35a2c7ac12028fd75f1b0573be132d784e938`.
All archived input hashes, effective platform configurations and structured
boundary records match iteration 204. Results remain one compiled no-NIC Rocket,
six compiler failures and six distinct unsupported partition/QSFP platforms.
Only the NIC diagnostic advances to read admission. Eight partial annotation
boundaries match, six retain the documented SRAM model-port differences and
twelve are not reached. All thirteen complete stderr logs were inspected.
The matrix exits nonzero because these implementation gaps remain; a matched
partial boundary does not establish target support. No FireSim manager gate was
started by this implementation turn.

Older mutable iteration 202 replay MLIR was gzip-compressed with verified
decompressed SHA256 and a manifest: 559 files, recovering 35,654,450,081 bytes.
Current candidates and completed SFC goldens were not compressed or modified.

The next smallest step is to bind NIC `FASEDReadAdmission`, closing AR readiness
with pending-read fullness and retiring on accepted final R beats. Compare its
counter, consumed ready inputs and annotation transfer to SFC before attaching
runtime request-limit MMIO. Full NIC host/control/platform composition,
generalized FASED flight counts/geometry, multiple/wider TracerV profiles,
recorded clock-gate shim behavior and partition/QSFP integration remain pending.


## Iteration 206 — NIC AR admission and final-R retirement (2026-10-14)

The native compiler now invokes `bindFASEDReadAdmission` on the archived NIC
composition and emits `post-fame-fased-read-admission.mlir` plus its complete
annotation JSON before rejecting runtime request-limit MMIO. This advances an
actual CIRCT FIRRTL operation boundary. NIC compilation still fails explicitly:
`NIC FASED request limits, control responses and platform integration are not implemented`.
No SFC fallback or FireSim manager verification step was started.

The executable Scala oracle is `TimingModel.scala` lines 104–106 and 226–231:
AR.fire increments pending reads; R.fire && R.last retires one; AR.ready is
!pendingReads.full. The native helper uses one four-bit host-clocked counter,
qualified reset/state updates, simultaneous-inc/dec hold, empty saturation,
full comparison against the live limit and no state clamp when the limit falls.
Eight internal observation ports carry the actual releaser final-R handshake.
Both the consumed and observed AR.ready use the same counter result. AW/W and
request payloads retain their existing connections.

Changed core files are `tools/goldengate.cpp`,
`test/Transforms/FASEDReadAdmissionTest.cpp`,
`test/Transforms/NICFASEDReadAdmissionCompare.py`, and this ledger. The C++ test
now validates the production feedback and eight observation hops, clones the
actual two final-R AndPrimOps, original counter-input/output connections and
entire helper body into a native probe, and verifies that probe before export.
The comparison checks the production composition, recursive annotation transfer
and exact shared helper body against the fresh no-NIC boundary.

Exact NIC SFC artifact compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Its `LatencyPipe`, `IdentityModule`, `AXI4Releaser`, `SatUpDownCounter_1` and
`FASEDMemoryTimingModel` define the tested boundary. The extracted counter,
AR/final-R equations and recorded clock-gate contract also match the immutable
U250 build-tree `sfc-golden-2026-10-01/design/FireSim-generated.sv` fixture.
The SFC counter module, original instance and assignments execute verbatim.
Only the surrounding shell and proven IdentityModule aliases are collapsed.

Fresh NIC candidate SHA256 `67307836ee73988779f4b05d2aaae865d4838ea83b73d82ca0fe03c57284547d`.
Comparison retained 185 unaffected modules, checked all
8 observation hops and 1607
prior/observation connections, 205 new wrapper
connections and all 1792 annotation records.
Both implementations close their own AR readiness back into acceptance.
The emitted production operations and SFC counter passed 40,231
Verilator differential cycles with outputs/registers checked before and after
every edge. All 16 limits, 16 counter states and 32 reset/fire/R-ready/valid/last
contexts were covered. Observed coverage includes 4323 accepted
ARs, 2636 accepted final R beats, 4978
non-final R beats, 11498 blocked ARs, 176
empty retirements, 549 simultaneous inc/dec cases,
20268 lowered-limit cases and 184 stalled
resets. The independent no-NIC preflight passed the same differential.
An initial stimulus coverage failure (48 empty retirements) was corrected by a
directed underflow sequence; no behavioral mismatch was observed.

Native compiler/test build and `goldengate-fased-read-admission` CTest passed,
including 27 atomic rejection cases in each of legacy and selected profiles.
Three production mutations passed native IR verification and were rejected by
the export contract: wrong retirement feedback, missing final-beat qualification
and mismatched observed AR readiness. Focused reports and full logs are in
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration206-u250-target-matrix` (not a golden fixture).

The compiler SHA256 is `a764aa287ebdae563096e62712488f87aff43dfc2cec5f0a3abe03b031c7457c` and the archive manifest SHA256
is `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`. All 13 archived targets replayed with their recorded
FIRRTL/annotation hashes and effective U250 platforms. Statuses remain one
compiled, six failed, six unsupported platforms. All complete stderr logs were
inspected. Boundary records remain eight matched, six SRAM model-port
mismatches, twelve not reached; these are partial checks, not support claims.
All 208 earlier NIC MLIR/JSON artifacts are byte-identical to iteration 205;
only the new read-admission MLIR/JSON pair was added. The no-NIC final RTL,
C/C++ header, Verilog header and both XDC outputs remain byte-identical.
Immutable primary SFC handoff hashes and normalized U250 XDC were rechecked.

Mutable iteration 203 replay MLIR was gzip-compressed with a manifest and
verified decompressed SHA256: 560 files, recovering 35,698,790,037 bytes. Completed
SFC goldens and current candidates were not compressed or modified.

The next smallest step is NIC request-limit MMIO attachment: compare host-clock
read/write maxima, reset default 10, full-word readback and low-four-bit admission
binding against the SFC FASED register cone before moving the guard. Runtime
limits, control responses, complete NIC host/platform integration, generalized
FASED geometry/flight counts, wider/multiple TracerV profiles and partition/QSFP
integration remain pending. This differential excludes completion-queue execution
and the recorded enable-register shim's delta-cycle behavior. The overall port
and pending SFC UART-bearing regression baseline remain incomplete.


## Iteration 207 — NIC request-limit MMIO and admission binding (2026-10-14)

The actual native compiler now invokes `attachFASEDRequestLimits` on the NIC
composition and emits `post-fame-fased-request-limits.mlir` and complete
annotation JSON before rejecting latency MMIO. Its explicit remaining guard is
`NIC FASED latency registers, control responses and platform integration are not implemented`.
No SFC fallback or FireSim manager verification step was started.

The executable Scala oracle is `SplitTransactionMMRegIO.maxReqRegisters` in
`TimingModel.scala`, `Widget.attachIO/genAndAttachReg`, and `MCRIO.bindReg` in
`widgets/Lib.scala`. Host-clock registers reset to 10, accept independent full
32-bit writes without strobes or target-fire qualification, return full words,
and drive the low four bits into the existing pending-read/write admission.
The FAME host reset/clock distinction was checked in `FAMETransform.scala`;
`MidasTransforms.scala` remains the preserved pass-order oracle.

Changed files are `tools/goldengate.cpp`,
`test/Transforms/FASEDRequestLimitsTest.cpp`,
`test/Transforms/NICFASEDRequestLimitsCompare.py`, and this ledger.
The C++ exporter verifies the real production IR, checks host reset/clock and
both consumed admission inputs, clones the entire native bank verbatim, and
clones its five original clock/reset/admission/MCR connections into a probe.
The comparison executes that emitted probe with firtool and Verilator against
original SFC register declarations, host-edge updates, readback and low-bit
assignments. Constant MCR valid/ready outputs, optimized away in SFC RTL, are
checked against the Scala `bindReg` contract.

Exact archived NIC SFC artifacts compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h`
SHA256 `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
The `FASEDMemoryTimingModel` register cone matches the immutable U250 build-tree
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv` fixture and executes without a semantic mismatch.
The NIC driver's readable/writeable addresses 136/140 match native FASED base
128 plus offsets 8/12 (words 2/3). All three retained allocation-region tables
agree. The immutable build-tree has no corresponding `.const.h`; the driver
address comparison uses the completed NIC archive header above.

Fresh NIC candidate SHA256 `6f233d2cbcd9258535a81ae9117ec03f10904981d6f3a67e08ab93ad09ff5603`.
All 195 prior modules and 1792
annotation records are preserved; 198 wrapper
connections and 193 copied ports match the expected composition.
Only the two explicit maximum inputs are consumed. The helper body exactly
matches the fresh no-NIC boundary. Verilator differential testing passed
32,849 cycles, checking outputs and registers before and after every
edge. All 16 strobes, all four independent write-valid masks, and all 16 limits
on each admission input were exercised. Coverage includes 32,869 writes,
1,561 zero-strobe writes, 790 reset/write overlaps,
16,445 upper-bit writes, 12,431 writes during target stalls,
and 24,692 read-backpressure samples.

Native compiler/test build and `goldengate-fased-request-limits` CTest passed:
65,536 cycles across legacy/selected profiles and 68 atomic rejection cases.
The independent no-NIC preflight passed the same RTL differential. Three
production mutations passed FIRRTL verification and were then rejected:
model reset instead of host reset, crossed admission maxima, and missing MCR
connection. An initial collateral check assumed a single region table; it was
corrected to check agreement across the three actual copies. The failed attempt
to read an absent immutable header was corrected to use the archived NIC header.
No RTL behavioral mismatch was observed.

Compiler SHA256 `014e2302e98ce6e89d4fc876258b9e7ec211e76ad57ff6d3b484ae7d40407f21`;
archive manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All 13 recorded U250 recipes replayed with their exact input hashes and effective
platforms, including all six distinct partition configurations. Statuses remain
one compiled, six failed, six unsupported platforms. Complete stderr logs were
read. Partial boundary records remain eight matched, six SRAM model-port
mismatches and twelve not reached; these are not semantic support claims.
The 210 earlier NIC MLIR/JSON artifacts are byte-identical to iteration 206;
only the request-limit MLIR/JSON pair was added. No-NIC final SV, C/C++ header,
Verilog header and both XDC outputs are byte-identical. Immutable primary SFC
handoff hashes and normalized U250 XDC were rechecked.

Reports and full focused logs are in `/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration207-u250-target-matrix`.
Mutable iteration 204 replay MLIR was gzip-compressed with verified decompressed
SHA256 and a manifest: 561 files, recovering 35,743,183,854 bytes.
Completed goldens and current candidates were not compressed or modified.

The next smallest step is NIC latency-register attachment: compare host-clock
read/write latency MMIO defaults, readback and timing-pipe bindings to SFC.
Control response routing and complete NIC platform integration, generalized
FASED flight counts/geometry, wider/multiple TracerV profiles and partition/QSFP
integration remain pending. This differential isolates decoded MCR fragments;
full control transport, admission-counter execution and complete NIC workloads
remain separate validation boundaries. The overall port and the pending SFC
UART-bearing regression baseline remain incomplete.

## Iteration 208: NIC FASED latency MMIO attachment

The production compiler now invokes `attachFASEDLatencyRegisters` for NIC
composition and emits verified `post-fame-fased-latency-registers.mlir` plus
complete annotation JSON. The next explicit rejection is
`NIC FASED functional model register, control responses and platform integration are not implemented`.
No SFC fallback or FireSim manager verification step was started.

The Scala semantic oracle is `LatencyPipeMMRegIO.registers` in
`LatencyBandwidthPipe.scala`, `Widget.attachIO/genAndAttachReg`, and
`MCRIO.bindReg` in `widgets/Lib.scala`. Two host-clock registers reset to 30,
accept independent full 32-bit writes without byte-strobe or target-fire
qualification, return full words, and drive the full-width timing inputs.
`FAMETransform.scala` supplies the host-reset distinction and
`MidasTransforms.scala` remains the preserved pass-order oracle.

Changed files are `tools/goldengate.cpp`,
`test/Transforms/FASEDLatencyRegistersTest.cpp`,
`test/Transforms/NICFASEDLatencyRegistersCompare.py`, and this ledger.
The test exporter verifies the entire production IR, checks host clock/reset
and both timing-input sources, clones the entire original native bank, and
clones its five real wrapper connections into an executable probe. Three
valid-FIRRTL production mutations are rejected: model reset, crossed latency
sources, and missing MCR connection. The differential executes that probe
against original SFC register declarations, host-edge updates, readback and
full-width timing assignments. Constant MCR valid/ready outputs, optimized
away in SFC, are checked against the Scala `bindReg` contract.

Exact archived NIC golden artifacts compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h`
SHA256 `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
The extracted `FASEDMemoryTimingModel` latency cone also matches the immutable
U250 build-tree reference
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
The NIC driver addresses 128/132 match FASED base 128 plus offsets 0/4 (words
0/1); all three retained allocation tables agree. The immutable build-tree has
no `.const.h`; the ABI comparison uses the completed NIC archive header above.

Fresh NIC candidate SHA256
`10f5ade1b933b6e19f9d8b71a68e1444395d0a7fd9cf31d95e4d1e752693d8bf`.
All 196 prior modules and 1792 annotation records are preserved. The 192 copied
ports and 197 wrapper connections match; only two explicit latency inputs are
consumed. The bank body matches the fresh no-NIC boundary verbatim.
Verilator differential testing passed 32,849 cycles with output and internal
register comparisons before and after every edge. Coverage includes all 16
strobes, four independent write-valid masks, all low nibbles on both full-width
outputs, 32,785 writes, 1,500 zero-strobe writes, 783 reset/write overlaps,
16,417 upper-bit writes, 12,585 writes during target stalls, and 24,584
read-backpressure samples. No RTL behavioral mismatch was observed.

The native compiler/test build and `goldengate-fased-latency-registers` CTest
passed: 65,536 cycles across legacy/selected profiles and 70 atomic rejection
cases. An independent no-NIC production-boundary preflight passed the same
32,849-cycle differential and three wiring-mutation checks.

Compiler SHA256 `df4af65a78dc057b8adec1fd759731809a35acf8991ed7bb6e7a898991e6c7c8`;
archive manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All 13 recipes replayed with their exact archived inputs and effective platform
configurations, including six distinct partition configurations. All complete
stderr logs were inspected. Statuses remain one compiled, six failed, six
unsupported platforms. Partial comparison records remain eight matched, six
SRAM model-port mismatches and twelve not reached; these are not support claims.
Only the NIC rejection advanced to the next attachment boundary. The 212 prior
NIC MLIR/JSON files are byte-identical to iteration 207; only the latency-register
MLIR/JSON pair was added. No-NIC final SV, C/C++ header, Verilog header and both
XDC outputs are byte-identical. Immutable primary SFC handoff hashes and
normalized U250 XDC were rechecked successfully.

Reports, candidates and full focused logs are in
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration208-u250-target-matrix`.
Mutable iteration 205 replay MLIR was gzip-compressed with verified decompressed
SHA256 and a manifest: 562 files, recovering 35,787,634,039 bytes.
Completed goldens and current candidates were preserved.

The next smallest step is NIC functional-model control-register attachment:
compare the host-clock register's zero default, full-word readback and bit-zero
binding to ingress relaxation against SFC. Control responses and complete NIC
platform integration, generalized FASED geometry/flight counts, wider/multiple
TracerV and partition/QSFP integration remain pending. This test isolates
decoded MCR fragments; full control transport, timing-queue execution and NIC
workload behavior remain separate validation boundaries. The overall port and
the pending SFC UART-bearing regression baseline remain incomplete. Harness
iteration 207 passed CIRCT replacertl and fresh bare-metal Verilator smoke;
those results precede this compiler change and do not validate iteration 208.

## Iteration 209: NIC FASED functional-model control attachment

The production compiler now invokes `attachFASEDFunctionalModelRegister` for
NIC composition, verifies the resulting FIRRTL operations, and emits
`post-fame-fased-functional-model-register.mlir` plus complete annotation JSON.
The next explicit rejection is
`NIC FASED response error registers, control responses and platform integration are not implemented`.
No SFC fallback or FireSim manager verification step was started.

The Scala semantic oracle is `FuncModelProgrammableRegs.registers` and
`ingress.io.relaxed` in `FASEDMemoryTimingModel.scala`, `Widget.attachIO` and
`genAndAttachReg`, and `MCRIO.bindReg`. SFC retains a host-clocked 32-bit word
with zero reset default, full-word writes and readback, and only bit zero
connected to ingress relaxation. Writes ignore strobes and target-fire;
read-valid/write-ready are unconditional. `FAMETransform.scala` supplies the
host-reset distinction and `MidasTransforms.scala` remains the pass-order
oracle; both Scala sources are preserved.

Changed files: `tools/goldengate.cpp`,
`test/Transforms/FASEDFunctionalModelRegisterTest.cpp`,
`test/Transforms/NICFASEDFunctionalModelRegisterCompare.py`, and this ledger.
The native exporter verifies the entire production IR, checks host clock/reset
and ingress-relaxation sources, clones the original bank verbatim, and clones
its four real wrapper connections into the executable probe. Three valid
FIRRTL mutations are rejected: model reset, wrong ingress-relaxation source,
and missing MCR connection. The differential executes the probe against
verbatim SFC register declarations, host-edge updates, full-word readback and
bit-zero ingress assignment. Constant MCR handshakes, optimized away in SFC,
are checked against the Scala `bindReg` contract.

Exact archived NIC golden RTL compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Exact driver ABI golden compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h`
SHA256 `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
The extracted `FASEDMemoryTimingModel` cone also matches the immutable U250
build-tree RTL:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`relaxFunctionalModel` is readable/writeable at address 200, matching FASED
base 128 plus offset 72 (word 18); all 3
retained allocation tables agree. The immutable build-tree has no `.const.h`;
the ABI comparison uses the completed NIC archive header above.

Fresh NIC candidate SHA256 `297ffd6b6c1aa93ba0cf3ad8ba005f8452f6367c2f276e041a85bb5672b9f222`.
All 197 prior modules and
1792 annotation records are preserved.
The 192 copied ports and
196 wrapper connections match; only the
explicit ingress-relaxation input is consumed. The bank body matches the fresh
no-NIC boundary verbatim. Verilator differential testing passed 32,809
cycles, comparing outputs and internal state before and after every edge.
Coverage includes all 16 strobes and both write-valid/relaxation states,
16,323 writes, 987 zero-strobe writes,
525 reset/write overlaps, 16,321 upper-bit writes,
8,119 writes during target stalls,
16,423 read-backpressure samples, and
15,306 nonzero even words with relaxation
correctly disabled. No RTL behavioral mismatch was observed.

The native compiler/test build and
`goldengate-fased-functional-model-register` CTest passed: 65,536 cycles across
legacy/selected profiles and 68 atomic rejections. An independent no-NIC
production-boundary preflight passed the same 32,809-cycle differential
and three wiring-mutation checks.

Compiler SHA256 `1a326e1c0c8e8f4ff29a7bc831524ecf3929f19496fe22fbb9cca74d4617ebef`;
archive manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All 13 recipes replayed with exact archived inputs and effective platform
configurations, including six distinct partition configurations. All complete
stderr logs were inspected. Statuses remain one compiled, six failed and six
unsupported platforms. Partial comparison records remain eight matched, six
SRAM model-port mismatches and twelve not reached; these are not support claims.
Only the NIC rejection advanced to the next attachment boundary. The
214 prior NIC MLIR/JSON files are byte-identical
to iteration 208; only the functional-model-register MLIR/JSON pair was added.
No-NIC final SV, C/C++ header, Verilog header and both XDC files are byte-identical.
Immutable primary SFC handoff hashes and normalized U250 XDC were rechecked.

Reports, candidates and full focused logs:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration209-u250-target-matrix`.
Mutable iteration 206 replay/probe MLIR was gzip-compressed with verified
SHA256 after decompression and a manifest: 570 files,
40,249,515,342 original bytes. Completed goldens and
current candidates were preserved.

The next smallest step is NIC response-error register attachment, comparing
accepted R/B handshakes, host-reset priority, read-only MMIO, and the SFC
B-error capture of the current R-response code. Control transport, ingress
queue execution, complete NIC platform integration and NIC workloads remain
separate validation boundaries. Generalized FASED geometry/flight counts,
wider/multiple TracerV, partition/QSFP integration and the SFC UART-bearing
regression baseline remain incomplete. Harness iteration 208 passed CIRCT
replacertl and fresh bare-metal Verilator smoke; those results precede this
compiler change and do not validate iteration 209. The overall port is
incomplete.

## Iteration 210: NIC FASED response-error attachment

The production compiler now invokes `attachFASEDResponseErrors` for NIC
composition, verifies the resulting FIRRTL operations, and emits
`post-fame-fased-response-errors.mlir` plus complete annotation JSON.
The next explicit rejection is
`NIC FASED statistics, control responses and platform integration are not implemented`.
The Scala oracle is preserved: `FASEDMemoryTimingModel.scala:580-587` uses
host-clock/reset error registers, captures accepted nonzero R/B errors, and
captures the R response code for B errors as well. `MCRIO.bindReg` establishes
the read-only lanes; `MidasTransforms.scala` and `FAMETransform.scala` remain
the pass-order and host/target reset references. No SFC fallback or FireSim
manager verification step was started.

Changed files: `tools/goldengate.cpp`,
`test/Transforms/FASEDResponseErrorsTest.cpp`,
`test/Transforms/NICFASEDResponseErrorsCompare.py`, and this ledger.
The native exporter verifies the whole production IR, validates the exact
host clock/reset and R/B valid/ready/code SSA sources, clones the production
bank verbatim, and clones all nine observed wrapper connections. Four valid
FIRRTL wiring mutations are rejected: model reset, crossed response code,
wrong ready observation, and missing MCR connection. The isolated probe
exposes observed response and decoded MCR inputs; response queues and the
control decoder are outside this behavioral comparison.

Exact NIC SFC RTL compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Exact SFC driver ABI compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h`
SHA256 `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`.
The extracted register declarations, updates, readback and assertion blocks
also match the immutable U250 build-tree RTL:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`
SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.
SFC readable/writeable address vectors agree with the native read-only
`rrespError`/`brespError` lanes at offsets 76/80 and addresses 204/208.
All 3 allocation tables agree on FASED base 128, size 128 and slave 1.

Fresh NIC candidate SHA256 `d46dc187d71f917631b4bac018fd569b2fddcafcd4e1c7430d601e9ac7fb9b70`.
All 198 prior modules, 1792 annotation records,
193 copied ports and 202 wrapper connections compare successfully.
The bank body matches the fresh no-NIC boundary verbatim. The Verilator
comparison passed 33,281 legal-input cycles, checking outputs and internal
state before and after every edge. Coverage includes all 16 R/B response-code
pairs, all four valid pairs and reset/write masks, 11,994 R captures,
11,923 B captures, 2,951 B captures of a zero R code,
8,899 B captures of a distinct R code, 9,646 captures with
the test target-fire flag low, and 25,145 read-backpressure samples.
This executes SFC's always-ready response boundary; native CTest additionally
enumerates stalled ready handshakes. Constant MCR valid/ready outputs are
checked against Scala `bindReg`, since SFC optimizes them away.

All three illegal-write masks produce the expected read-only diagnostics and
are suppressed during host reset in both implementations. A separate
**termination-policy mismatch** is observed: SFC's emitted `$fwrite` prints
and continues, while CIRCT's existing assertion stops simulation. Predicates
and messages match; termination parity is not claimed. Both retained probe
reports and `assertion-policy-observation.json` record this difference from
the complete native/oracle negative-run logs. This is a remaining compiler
semantic gap, not a passing full assertion comparison.

The native compiler/test build and `goldengate-fased-response-errors` CTest
passed: 65,536 cycles across legacy/selected profiles, split/combined
construction, 68 phase rejection checks and 38 boundary rejection checks.
An independent no-NIC production-boundary preflight passed the same
33,281-cycle legal RTL differential, four mutation checks and three
illegal-write diagnostic masks.

Compiler SHA256 `4f34b7e1b55febc58abdff2d130b68941964b6375e2a6df920e40ca65ae52b98`;
manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All 13 archived recipes replayed with exact input hashes and effective
platform configurations, including six distinct partition configurations.
Complete stderr logs for all 13 were inspected. Statuses remain one compiled,
six failed and six unsupported platforms. Partial annotation comparisons
remain eight matched, six SRAM model-port mismatches and twelve not reached;
these are not complete target support claims. Only NIC's explicit rejection
advanced. Its 216 prior MLIR/JSON files are byte-identical to iteration 209;
only the response-error MLIR/JSON pair was added. No-NIC final SV, C/C++ header,
Verilog header and both XDC files are byte-identical. Immutable primary SFC
handoff hashes and normalized U250 XDC were rechecked successfully.

Reports, candidates and focused build/export/mutation/Verilator logs:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration210-u250-target-matrix`.
No storage cleanup was needed after the user's recorded cleanup; immutable
fixtures and previous retained candidates were preserved.

The next smallest change is to match the SFC response-error read-only
assertion termination behavior, using the retained illegal-write logs as the
oracle, before attaching NIC target transaction/beat statistics. Full control
transport, response queues and NIC workloads remain separate validation
boundaries. Generalized FASED flight/geometry profiles, wider/multiple TracerV,
partition/QSFP integration and the SFC UART-bearing baseline remain pending.
Harness iteration 209 passed CIRCT replacertl, fresh smoke, the 90,141-check
portable Spike suite and the 90,805-check UART-bearing Rocket suite; those
results precede this change and do not validate iteration 210. The overall
port remains incomplete.


## Iteration 211: preserve nonfatal response-error diagnostics

`FASEDResponseErrors.cpp` now emits two FIRRTL `PrintFOp` operations instead
of fatal `AssertOp` operations for read-only response-error writes. Each uses
the existing host clock and `!hostReset && write.valid` guard. This ports the
observed SFC host RTL policy: diagnose every illegal-write edge and continue
simulation, including reporting both lanes when both are written. Response
capture, host-reset priority, readback, permissions and wrapper connections
are unchanged. The Scala oracle remains untouched: `FASEDMemoryTimingModel.scala:580-587`
captures accepted nonzero errors (including R-code capture for B errors),
`MCRIO.bindReg` in `widgets/Lib.scala:281-297` defines read-only lanes, and
`MidasTransforms.scala` / `FAMETransform.scala` supply pass-order and
host/target clock/reset context. Other host assertions require separate
oracle comparisons; this change applies only to these two registers.

Changed files: `lib/Transforms/FASEDResponseErrors.cpp`,
`test/Transforms/FASEDResponseErrorsTest.cpp`,
`test/Transforms/NICFASEDResponseErrorsCompare.py`, and this ledger.
The comparator inspects the raw SFC FASED module before stripping macro
wrappers and rejects any `$fatal`, `$finish` or `$stop` there. Its original
register updates, readback and `$fwrite` blocks match the immutable U250
reference. Both emitted implementations use simulation-only `PRINTF_COND`
guards. The native diagnostic retains the canonical assertion message; SFC's
additional Scala source excerpt is not reproduced or claimed byte-identical.

Exact NIC SFC RTL compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Exact immutable U250 RTL compared:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`
SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.
The NIC SFC `FireSim-generated.const.h` at the same matrix design directory,
SHA256 `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`,
agrees with all three native allocation tables: FASED base 128, size 128,
slave 1; read-only `rrespError` / `brespError` offsets 76/80, addresses 204/208.

Native compiler/test build and `goldengate-fased-response-errors` CTest passed:
65,536 cycles across legacy/selected profiles, split/combined construction,
68 phase rejection checks and 38 boundary rejection checks. Fresh NIC and
no-NIC production-boundary Verilator probes each passed 33,281 legal-input
cycles plus nine illegal-write runs (three masks, each native-only,
oracle-only and joint). Each active lane diagnoses exactly three successive
edges; reset writes are silent; joint state/readback checks confirm continued
capture and recovery after writes and subsequent host reset. All runs exit
normally. The prior fatal/native versus nonfatal/SFC mismatch is resolved at
this boundary. Each probe also rejects four valid-IR production wiring
mutations after whole-IR verification. SFC's optimized always-ready boundary
is used in the RTL differential; native CTest separately covers ready stalls.
Full response queues, decoder transport, and NIC workload behavior are outside
these isolated probes.

The NIC comparison preserves 198 prior modules, 193 copied ports,
202 wrapper connections and 1,792 annotation records. Its production bank
matches fresh no-NIC verbatim. NIC candidate SHA256
`c3a71bdd74d8b76a2ccfd151e0aa9a578d26001370452938469dd81e01735cc6`;
no-NIC candidate SHA256
`9d96076f4623cc2727980245a9ddebfd7515b99f5da60396e20897569ac13b60`.
Against iteration 210, 218 NIC MLIR/JSON files and 256 no-NIC files were
checked. Only bank bodies differ in 56 NIC and 78 no-NIC MLIR boundaries,
after normalizing the candidate-directory annotation; JSON records and all
other module operations remain unchanged. Of 1,464 no-NIC emitted RTL
modules, only `GGFASEDResponseErrors` changes. C/C++ and Verilog headers and
both XDC files are byte-identical. No-NIC SV SHA256
`b242eefbb1b1e2039ede11e65896d158b53305d45b776a08e5a89cec7d9bdb37`.
Immutable primary FIRRTL/annotation hashes and normalized U250 XDC were
rechecked successfully. Neither immutable fixture was modified.

Compiler SHA256 `0b80d6b98f096eb2f0512f3f05f28022bf3d0436218813954f4d3d9cd48818b9`;
manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All thirteen archived recipes replayed with unchanged exact input hashes,
effective partition/platform configurations, statuses and structured boundary
comparisons. Complete stderr logs were inspected. Results remain one compiled,
six failed and six unsupported platforms. Annotation comparisons remain eight
matched, six SRAM model-port mismatches and twelve not reached. Matrix exit 1
records these implementation gaps, not a passing generalized compiler.
The NIC rejection still names statistics, control responses and platform
integration. Other failures retain multiple/wider TracerV and FASED maxFlight
profile restrictions; partition/QSFP configurations remain unsupported.

Fresh reports, candidates and build/export/mutation/Verilator logs:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration211-u250-target-matrix`.
Reports include `nic-response-errors-comparison.json`,
`no-nic-response-errors-comparison.json`, `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`, and
`matrix-regression.json`. Existing user changes and retained candidates were
preserved; no storage cleanup was needed.

The next smallest porting step is to attach NIC FASED target transaction/beat
statistics and compare their counters and decoded read-only ABI against SFC,
including their own diagnostic policy. Control responses, platform integration,
generalized FASED/TracerV profiles, partition/QSFP behavior and the SFC
UART-bearing baseline remain pending. Harness iteration 210 passed CIRCT
replacertl and fresh bare-metal smoke with unchanged-input long-suite evidence
reused; those results precede this change. No FireSim manager verification
step was started here, and iteration 211 manager validation remains pending.
The overall port remains incomplete.


## Iteration 212: NIC FASED transaction/beat statistics and host diagnostics

The native NIC compiler now attaches `GGFASEDStatisticsWrapper` after the
response-error boundary, exposing accepted target R beats through the existing
13-module response chain and binding four read-only counters. The next explicit
NIC rejection names histograms, control responses and platform integration.
This moves the actual CIRCT compiler forward; NIC end-to-end compilation still
fails and is not counted as supported.

`models/dram/TimingModel.scala:151-166` counts accepted W/R beats and AW/AR
transactions, wrapping at 32 bits. `widgets/Lib.scala:281-297` supplies the
read-only register contract; the recorded SFC FASED RTL emits host-clock
`$fwrite` diagnostics without `$fatal`, `$finish` or `$stop`. The native bank
now uses four `firrtl.printf` operations for those writes, with host-reset
suppression independent of target fire and model reset. Counter reset/update
remains gated by target fire. The canonical assertion message is retained;
SFC's additional Scala source excerpt is not reproduced byte-for-byte.
`MidasTransforms.scala` / `FAMETransform.scala` were inspected for pass order
and host/target clock/reset context. Other banks' diagnostic policies remain
separate oracle comparisons.

Changed files: `include/goldengate/FASEDStatistics.h`,
`lib/Transforms/FASEDStatistics.cpp`, `tools/goldengate.cpp`,
`test/Transforms/FASEDStatisticsTest.cpp`,
`test/Transforms/NICFASEDStatisticsCompare.py`, and this ledger.
The native exporter verifies the complete production IR and its eight
clock/reset/fire/event inputs plus MCR output before cloning the bank and
nine connections verbatim. The comparator checks the original SFC model
instance's gated clock/reset, counter clocked updates, and independent
host-clock diagnostic blocks rather than reconstructing Scala behavior.

Exact NIC SFC RTL compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Its extracted statistics and diagnostic cones match the immutable U250 RTL:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`
SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.
The same-directory NIC SFC `FireSim-generated.const.h`, SHA256
`ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`,
agrees with every native allocation-table copy: FASED base 128, size 128,
slave 1; `totalWriteBeats`, `totalReadBeats`, `totalWrites`, `totalReads`
read-only offsets 56/60/64/68, addresses 184/188/192/196. The no-NIC archived
SFC RTL at `u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`,
and its header, SHA256
`34a3d08b9cf8b179eb0f951e23e187c37d30ff1ea084aa76058e704f5737a308`,
match the same cones and register ABI. Neither immutable fixture was modified.

Native compiler/test build and `goldengate-fased-statistics` CTest passed:
65,536 cycles across legacy/selected profiles, split/combined construction,
34 phase rejection checks per profile and 52 boundary rejection checks.
Fresh NIC and no-NIC production-boundary Verilator probes each passed 34,817
legal-input cycles, including 1,495 counter wraps, 1,058 stalled model resets,
16,280 host-only resets, 8,873 blocked R beats and 2,080 accepted nonfinal R
beats. Each additionally passes fifteen illegal-write runs: five masks,
each native-only, oracle-only and joint. Each active lane diagnoses three
successive host edges, including target stalls/model reset, and jointly six
messages; host-reset writes are silent. Counter continuation, recovery and
subsequent stalled/firing model resets match. All runs exit normally. Each
probe rejects three valid-IR production wiring mutations after full IR
verification. Full queues, decoder transport and NIC workload behavior remain
outside these decoded-MMIO/observed-handshake probes.

The NIC comparison preserves 186 unrelated module bodies, 194 copied ports,
203 wrapper connections and 1,792 annotation records; thirteen response
modules retain their existing drivers while exposing accepted R. The bank
matches fresh no-NIC verbatim. NIC candidate SHA256
`d78c2ef1bfd69070741356fe7ffbf3ade9aa4460f9c7bba951fe4a3c2a29b8f3`;
no-NIC statistics candidate SHA256
`cf42a1badb97c7970c8d0719d2747cc9c00a16b50f16d2ddca64e65d307db3ec`.

Against iteration 211, 218 NIC and 256 no-NIC shared MLIR/JSON files were
checked after normalizing the candidate-directory annotation. Only statistics
bank bodies differ in 55 NIC and 77 no-NIC MLIR files; shared JSON records
and all other operations remain unchanged. NIC adds precisely its statistics
MLIR and annotation boundaries. Of 1,464 emitted no-NIC RTL modules, only
`GGFASEDStatistics` changes. Headers and both XDC files are byte-identical;
no-NIC SV SHA256
`094eae405f50c93881916d85768712919b8fc6d6e33376c0ee9418f78924eafb`.
Primary immutable FIRRTL/annotation hashes and normalized U250 XDC also
match. The first no-NIC comparison attempted a nonexistent adjacent immutable
header; the retained failure log is followed by successful runs using the
archived no-NIC header plus the immutable RTL cross-check. A regression-helper
recipe suffix error was corrected before its successful complete comparison.

Compiler SHA256 `3fb6a525b1b8e3bb4e0b025acb88575127e20fb7bb65b8e2072b224935691fca`;
manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All thirteen exact archived inputs and effective platform configurations were
replayed; complete stderr logs were inspected. Results remain one compiled,
six failed and six unsupported platforms. Structured annotation comparisons
remain eight matched, six SRAM model-port mismatches and twelve not reached,
with unchanged input hashes/statuses/checks. Exit 1 records these gaps.
NIC now rejects at histograms; other failures retain multiple/wider TracerV,
FASED maxFlight profile restrictions and partition/QSFP implementation gaps.

Fresh evidence directory:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration212-u250-target-matrix`.
Final reports are `nic-statistics-final-comparison.json`,
`no_nic-statistics-final-comparison.json`, `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`, and
`matrix-regression.json`. Final differential logs/probes use the `-v3` suffix;
`artifact-regression-v2.log` records the successful complete regression.
Existing user changes, failed probe logs and earlier candidates were preserved.
No storage cleanup was needed.

The next smallest porting step is NIC FASED occupancy histograms, with their
own gated counter/reset and decoded read-only ABI/diagnostic comparison.
NIC control responses/platform integration, generalized profiles, partition/QSFP
behavior and the SFC UART-bearing baseline remain pending. Harness iteration
211 passed actual CIRCT replacertl, fresh bare-metal smoke, 90,141 portable
Spike checks and 90,805 UART-bearing Rocket checks; those precede this change.
No manager verification step was started here; iteration 212 manager validation
remains pending. The overall port remains incomplete.

## Iteration 213: NIC FASED occupancy histograms and read-only diagnostics

The native compiler now attaches `GGFASEDHistogramsWrapper` on the NIC path
and emits its MLIR/annotation boundary before the explicit rejection for NIC
FASED control responses and platform integration. The histogram bank observes
pre-edge pending reads and AW requests, preserving the existing producer
hierarchy, host clock/reset, target reset/fire and all copied ports.
NIC end-to-end compilation remains an implementation gap.

`TimingModel.scala:169-192` allocates five lanes per histogram but zips updates
with four upper bounds (0, 2, 4, 8). The executable SFC output has eight wrapping
32-bit registers and two constant-zero fifth lanes; occupancies above eight
increment no lane. Target model reset and updates occur only on target fire.
`widgets/Lib.scala:281-297` supplies the decoded read-only register contract.
All ten illegal-write diagnostics in the recorded host RTL are host-clock
`$fwrite` calls, suppressed by host reset, with no `$fatal`, `$finish` or `$stop`.
The native bank now uses ten `firrtl.printf` operations with those enables;
model reset and stalled target fire do not suppress diagnostics. The canonical
message is retained; the appended Scala source excerpt is not byte-identical.
`MidasTransforms.scala` and `FAMETransform.scala` were inspected for actual
pass order and host/target clock/reset semantics; the Scala oracle is unchanged.

Changed files: `include/goldengate/FASEDHistograms.h`,
`lib/Transforms/FASEDHistograms.cpp`, `tools/goldengate.cpp`,
`test/Transforms/FASEDHistogramsTest.cpp`,
`test/Transforms/NICFASEDHistogramsCompare.py`, and this ledger.
The native exporter first verifies the complete production IR, its six input
bindings and MCR output, then clones the actual bank and seven connections
verbatim. The comparison extracts original SFC declarations, assignments,
clocked histogram updates and host diagnostic blocks. It checks the original
model instance's gated clock and reset, read-only global allocation, optimized
fifth lanes, copied wrapper connections and recursive annotation transfers.
Its probe isolates decoded MMIO and observed pre-edge occupancy; pending-count
producers, full decoder transport, NIC workloads and platform behavior remain
outside this differential's scope.

Exact NIC golden compared:
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
The histogram/diagnostic cones match the immutable U250 build-tree RTL at
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.
The no-NIC archived `design/FireSim-generated.sv`, SHA256
`2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`,
contains the same cones. Adjacent NIC/no-NIC `FireSim-generated.const.h` hashes
are respectively `ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`
and `34a3d08b9cf8b179eb0f951e23e187c37d30ff1ea084aa76058e704f5737a308`.
All ten histogram rows agree with every native allocation-table copy:
FASED base 128, size 128, slave 1, offsets 16-52, addresses 144-180,
read-only permissions including both zero fifth lanes.

The native compiler/test build and histogram CTest passed: 65,536 cycles across
legacy/selected profiles, 28 phase rejection checks per profile and 46 atomic
boundary rejections. Each production-boundary Verilator differential (NIC and
no-NIC) passed 40,961 cycles, 3,175 wrapping updates, 2,592 stalled model resets,
18,047 host-only resets and 13,915 firing samples above the last bound.
Each also passed 33 illegal-write runs: ten individual lanes plus the full mask,
each native-only, oracle-only and joint. Three active host edges diagnose writes
through target stalls/model reset; host-reset writes remain silent. Normal
execution, subsequent updates and stalled/firing target resets continue and
match. Both fifth readbacks remain zero. Three valid-IR production mutations
(host reset, target fire and AW replaced with W occupancy) are rejected after
full IR verification in each probe.

The NIC attachment preserves 200 existing modules, 195 copied ports, 202
connections and 1,792 annotation records. The no-NIC check preserves 186
modules, 190 copied ports, 197 connections and 1,676 annotation records.
Fresh NIC/no-NIC banks match verbatim. Histogram candidate SHA256 values are
`533bd2a79efd6e2a10d4ce3e655fff4caceb02e22d19f3ebb0538443fcc2cdcf`
(NIC) and `2bf43ee7efbb9efad45ea2eca4dbdd6b1986baa792cc417da4bb32f0df4432fb`
(no NIC).

Against iteration 212, 220 NIC and 256 no-NIC shared MLIR/JSON files were
checked after normalizing the candidate-directory annotation. Only histogram
bank bodies differ in 55 NIC and 76 no-NIC MLIR files; shared JSON records
and all other operations remain unchanged. NIC adds precisely its histogram
MLIR and annotation boundaries. Of 1,464 emitted no-NIC RTL modules, only
`GGFASEDHistograms` changes. Headers and both XDC files are byte-identical;
no-NIC SV SHA256
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
Primary immutable FIRRTL/annotation hashes and normalized U250 XDC also
match. The artifact regression first ran before the final matrix report was
available; that failure log is retained alongside the completed comparisons.

Compiler SHA256 `1321972c87a22b388d7038b285a8d314a1ee69934bb67889a1054b8fa1534551`;
manifest SHA256 `0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All thirteen exact archived inputs and effective platform configurations were
replayed; complete stderr logs were inspected. Results remain one compiled,
six failed and six unsupported platforms. Structured annotation comparisons
remain eight matched, six SRAM model-port mismatches and twelve not reached,
with unchanged input hashes, statuses and comparison checks. Exit 1 records
these implementation gaps. NIC now rejects at control responses/platform
integration; other failures retain multiple/wider TracerV, FASED maxFlight
profile restrictions and partition/QSFP implementation gaps.

Fresh evidence directory:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration213-u250-target-matrix`.
Reports are `nic-histograms-comparison.json`,
`no_nic-histograms-comparison.json`, `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`matrix-regression.json`. `artifact-regression-v2.log` records the successful
complete regression. `build.log`, `ctest.log`, `native-test.log`, per-recipe
replay logs, and both `*-histograms-probe` directories retain the actual
compiler, differential, diagnostic and mutation evidence. Existing user
changes and the premature comparison failure log were preserved; no storage
cleanup or immutable-fixture modification was needed.

The next smallest porting step is the NIC FASED 21-word aggregate MMIO bank,
comparing all six fragment mappings and decoded read/write behavior against
SFC `MCRFile_5`, followed by control responses. NIC runtime/platform integration,
generalized profiles, partition/QSFP behavior and the SFC UART-bearing baseline
remain pending. Harness iteration 212 passed actual CIRCT replacertl, fresh
bare-metal smoke, 90,141 portable Spike checks and 90,805 UART-bearing Rocket
checks; those precede this change. No manager verification step was started
here; iteration 213 manager validation remains pending. The overall port
remains incomplete.

### Iteration 214: NIC FASED aggregate MMIO lanes

The actual NIC compiler path now calls `addFASEDMMIOBank` after the six FASED
register fragments. It emits and verifies `post-fame-fased-mmio.mlir` and
`post-fame-fased-mmio-all.json` before the explicit control-response/platform
rejection. This is an executable FIRRTL wrapper containing the 21 ordered
read/write token lanes and six write-strobe connections. The no-NIC path uses
the same existing pass. No Scala oracle or immutable fixture was modified.
`MidasTransforms.scala`, `FAMETransform.scala`, `Widget.attachIO/attach`, and
`FASEDMemoryTimingModel.attachIO/attach` were inspected for pass ordering,
host/target scheduling and the actual register-allocation semantics.

Changed files: `tools/goldengate.cpp`,
`test/Transforms/FASEDMMIOBankTest.cpp`,
`test/Transforms/NICFASEDMMIOBankCompare.py`, and this ledger. The native test
can verify the complete production boundary and clone its actual routing
operations into an isolated probe, replacing only the inner simulation's six
fragment ports with independent inputs. It preserves FIRRTL bundle flips and
introduces no substitute register-bank implementation or control decoder.

The exact NIC SFC artifacts compared are
`/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`)
and adjacent `FireSim-generated.const.h`
(`ea7517e1fed9d0062601d89b9d8d51a3af4639dd5be6dd9cc8a05d91a9bc1563`).
All 21 word names, read bindings, five writable words, read-only diagnostics,
two optimized zero histogram lanes and driver addresses agree with SFC
`FASEDMemoryTimingModel`/`MCRFile_5`: offsets 0–80, base 128, size 128, slave 1.
Both response-error readbacks use the oracle's two-bit zero extension.
The no-NIC archive's corresponding SV/header hashes are
`2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702` and
`34a3d08b9cf8b179eb0f951e23e187c37d30ff1ea084aa76058e704f5737a308`;
the same checks pass there. The immutable U250 build-tree reference's
`design/FireSim-generated.sv` at
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
(`20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`)
also passes the RTL map/permission checks. That build tree contains no header;
its address check explicitly uses the separately archived no-NIC SFC header.

The native build and MMIO CTest passed: legacy/selected profiles, three lane
orders each, 59 annotation targets and 188 atomic rejection cases. Fresh NIC
and no-NIC production probes each passed 11,058 isolated Verilator samples:
all-zero/all-one inputs, walking-one/walking-zero over 1,432 independent input
bits, then 8,192 randomized samples. Every data bit, valid/ready direction and
all six four-bit strobe routes are checked independently. A valid FIRRTL
mutation swapping aggregate lanes zero/one in both read and write vectors
lowers successfully and fails at sample 2, word 0 in each probe. This is a
combinational carrier check; register updates, Nasti control transport and
NIC runtime are outside this test's scope.

The NIC assembly preserves 201 prior module bodies, 190 copied typed ports,
238 connections and 1,792 recursively transferred annotation records. The
no-NIC assembly preserves 187 prior modules, 185 copied ports, 233 connections
and 1,676 annotation records. Each wrapper introduces exactly 48 aggregate
routes, no state, and exactly the expected global register metadata.

Evidence directory:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration214-u250-target-matrix`.
`nic-mmio-probe/comparison.json`, `no-nic-mmio-probe/comparison.json`, and
`immutable-fased-mmio-comparison.json` retain structured map/permission,
allocation, routing and annotation checks. Each probe directory retains its
actual cloned/lowered circuit, testbench, normal execution and failing mutation
logs. Preliminary checks on iteration 213's no-NIC boundary are retained in
`previous-no-nic-probe` and `mutation-probe`; the fresh probes are the current
iteration's execution evidence. `build.log` and `ctest.log` record native checks.

The next smallest porting step is NIC FASED MCRFile control responses and
binding, comparing decoded index behavior, backpressure, IDs, burst/last and
reset semantics against SFC `MCRFile_5`. NIC runtime/platform integration,
generalized profiles, partition/QSFP behavior and the SFC UART-bearing baseline
remain pending. Harness iteration 213 passed actual CIRCT replacertl, fresh
bare-metal smoke, 90,141 portable Spike checks and 90,805 UART-bearing Rocket
checks; those precede this change. No manager verification step was started
here; iteration 214 manager validation remains pending. The overall port is
incomplete.

The fresh thirteen-recipe replay used compiler SHA256
`b721e63918af7aa36081a865b5f512ff94850d8568086f45b460fdd28700e204` and
manifest SHA256
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All archived input hashes and effective platform configurations match iteration
213. Results remain one compiled, six failed and six unsupported platforms;
annotation comparisons remain eight matched, six SRAM model-port mismatches
and twelve not reached. Complete per-recipe stderr logs were inspected. All
failure messages and boundary comparison checks are unchanged. NIC reaches its
two new MMIO artifacts before its existing rejection; rejected targets remain
implementation gaps. The standalone matrix's exit 1 reflects those gaps.

`artifact-regression.log` records a passing complete regression. All 222 shared
NIC and 256 shared no-NIC MLIR/JSON files match iteration 213 after normalizing
only the candidate-directory annotation. NIC adds precisely its two MMIO
boundary files. No-NIC final RTL, both headers and both XDC files remain
byte-identical; SV SHA256
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
Primary immutable handoff FIRRTL/annotation hashes and normalized U250 XDC
also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`,
`matrix-regression.json`, `tool-input-hashes.json` and `native-test.log` retain
these checks. Existing user edits remain outside this checkpoint.

## Iteration 215: NIC FASED local MCRFile transport

The production compiler now invokes `mapFASEDBridgeControl` on the NIC
`GGFASEDMMIOWrapper`, validates the resulting adapter/fragment register count
against the allocated widget, verifies the full IR, and emits
`post-fame-fased-control.mlir` and `post-fame-fased-control-all.json`. This
attaches the existing CIRCT implementation of Scala `Lib.scala` MCRFile to
the real NIC bank. The following slave/platform boundary still rejects with
`NIC FASED control slave binding and platform integration are not implemented`.
This is local transport progress, not successful NIC compilation or runtime.

The Scala oracle inspected this turn was `MidasTransforms.scala`'s
InferModelPorts/FAMETransform/SimulationMapping ordering,
`FAMETransform.scala`'s host clock/reset and target/channel renames,
`Widget.scala:genCRFile`, and `Lib.scala:MCRFile`. The adapter retains separate
AW/W capture, held B/R flags and captured IDs, live read data, five-bit local
`addr[6:2]` selection, zero legacy MCR strobes, and handshake/reset-qualified
AW/AR single-beat assertions. It has four reset transaction flags and five
unreset payload/index registers. The NIC wrapper preserves 202 prior module
bodies, 190 copied typed ports and 1,792 annotation records; all 194 actual
connections and recursive annotation transfers pass. The no-NIC equivalent
preserves 188 modules, 185 copied ports, 1,676 annotations and 189 connections.
Both wrappers introduce no extra state beyond the one shared MCRFile.

`FASEDControlTest.cpp` can now export an exact clone of the production adapter
for isolated lowering; it does not rebuild a test implementation. The new
`NICFASEDControlCompare.py` compares the full attachment, state equations,
typed ports, annotation transfer, 21 register offsets/permissions and allocated
128-byte region at base 128/slave 1, then executes the cloned adapter against
the actual archived SFC `MCRFile_5` RTL. The golden artifacts are:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
  (SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`)
  and its `FireSim-generated.const.h` driver ABI.
- The corresponding `alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`
  (SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`)
  and its driver header, both in that same archive.
- The immutable October 1 U250 build-tree fixture's `design/FireSim-generated.sv`
  at `/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
  (SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`).
  That fixture has no driver header; the ABI comparison explicitly supplies
  the separately archived no-NIC SFC header above.

All three fresh production/SFC RTL differentials pass 24,898 isolated
Verilator cycles each: 3,904 reads, 365 writes, 248 AW-first transactions,
218 W-first transactions, 8,690 read-stall cycles and 1,528 write-stall cycles.
Directed and randomized traffic covers all 32 valid/invalid indices, high
address aliases/byte offsets, concurrent read/write requests, reset
cancellation, live bank read-data changes during stalls and ignored W metadata.
SFC has optimized constant bank handshakes, zero histogram bins 8/13 and
two-bit error words 19/20; the test specializes only those bank inputs.
The decoded register state machines and NIC platform remain outside this
transport differential. A valid FIRRTL mutation shifting local selection to
`addr[7:3]` lowers successfully and is caught at cycle 8 in all three probes.

The native compiler/test build and focused FASED control CTest pass. The CTest
covers 96,000 interpreted cycles over 1-, 21-, 32- and 33-word banks, register
backpressure, AW/W reordering, reset, assertion qualification, six nested
target transfers, 27 atomic boundary rejections and repeated-pass rejection.
`ctest-details.log` contains its successful complete output; a redundant
direct invocation was stopped after that CTest passed.

Evidence directory:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration215-u250-target-matrix`.
`nic-control-probe/comparison.json`, `no-nic-control-probe/comparison.json`,
and `immutable-control-probe/comparison.json` retain the structured checks,
actual cloned/lowered RTL, SFC oracle, testbench, passing execution and failing
mutation logs. Preliminary checks on iteration 214's no-NIC adapter are kept
separately and are not the fresh NIC evidence.

The next smallest porting step is to bind NIC FASED AW/W/AR requests and B/R
responses through the catalog-selected control slave, comparing the actual
dispatcher/arbiter connections with SFC `NastiRouter`/`FPGATop`. NIC platform
and runtime, generalized memory/TracerV profiles, partition/QSFP behavior,
SRAM model-port differences and the SFC UART-bearing baseline remain pending.
Harness iteration 214 passed actual CIRCT replacertl/provenance and fresh
bare-metal smoke; its long-suite results reused unchanged-input evidence.
Those checks precede this change. No FireSim manager verification step was
started here, and iteration 215 manager validation remains pending. The
overall compiler port is incomplete.

Fresh all-13 replay is complete in `iteration215-u250-target-matrix/replay/`.
Compiler SHA256 is
`4ef2cd4dc3c1fbac4394b11583a668a86375abe0d831675440c1ca610b91f2cc`;
archive manifest SHA256 is
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All archived input hashes and effective platform configurations match
iteration 214. Results remain one compiled, six failed and six unsupported
platforms; annotation comparisons remain eight matched, six SRAM model-port
mismatches and twelve not reached. Complete per-recipe stderr was inspected.
Only the NIC rejection advances to control slave binding. All other failure
messages and boundary comparison checks are unchanged. These rejections and
the matrix's exit 1 remain implementation gaps, not parity.

`artifact-regression.log` records a passing complete regression. All 224
shared NIC and 256 shared no-NIC MLIR/JSON files match iteration 214 after
normalizing only the candidate-directory annotation. NIC adds precisely the
two control boundary files. No-NIC final RTL, both headers and both XDC files
remain byte-identical; SV SHA256
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotations and normalized immutable
U250 XDC also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`,
`matrix-regression.json` and `tool-input-hashes.json` retain these checks.
Existing user edits remain outside this checkpoint.

## Iteration 216: NIC FASED catalog-selected slave binding

The production NIC path now invokes `bindFASEDBridgeControl` after its MCRFile
transport, verifies the full CIRCT module, and emits
`post-fame-fased-bound.mlir` and `post-fame-fased-bound-all.json`. This attaches
the existing native FIRRTL five-channel binding to the actual NIC boundary.
It selects FASED by the decoder catalog name rather than a hardcoded slot.
The explicit rejection moves to `NIC host control master and platform
integration are not implemented`; NIC still does not compile to platform RTL.

The Scala oracle inspected was the InferModelPorts/FAMETransform/SimulationMapping
ordering in `MidasTransforms.scala`, host clock/reset and channel renames in
`FAMETransform.scala`, and `Widget.scala:HasWidgets.genCtrlIO`: widgets sorted
by region size are zipped with recursive-interconnect slaves. The existing
native binding retains local AW address/length/ID and W data/last, carries
shared AW/W metadata, connects the whole AR bundle including flipped ready,
and returns B/R readiness from the response arbiters. Tracker retirement,
host clock/reset, register state and timing state remain unchanged.

`NICFASEDBindingCompare.py` checks every actual connection, copied typed port,
catalog identity and recursive raw/emitted annotation transfer. The fresh NIC
boundary preserves 204 prior module bodies, 168 copied ports and 1,792
annotation records, consumes exactly 23 boundary ports, and adds no state.
Its 201 connections comprise 168 copied ports, 32 scalar channel connections
and one aggregate AR connection with 13 leaves. All 33 channel connections
also match the fresh no-NIC binding. The no-NIC/immutable comparisons preserve
190 modules, 163 copied ports, 1,676 annotations and 196 connections.

Exact SFC boundary artifacts compared:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
  (SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`)
  and its `FireSim-generated.const.h`.
- The same archive's
  `alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`
  (SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`)
  and its driver header.
- The immutable October 1 U250 build-tree fixture's
  `design/FireSim-generated.sv` at
  `/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
  (SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`).
  Its absent driver header is explicitly supplied from the no-NIC archive.

All three structured comparisons pass. SFC `NastiRouter` AW/AR decode selects
`[128,256)`, and all 20 surviving `FPGATop`/`FASEDMemoryTimingModel` control
pins across AW/W/B/AR/R bind to slave 1, agreeing with the driver ABI. Native
IR retains metadata fields optimized away by SFC; all 45 leaves are checked
against the typed native interface and direct routing contract. This is a
stateless wiring comparison, not a new NIC behavioral or runtime result.

The native binding test adds the actual NIC allocation, twelve regions with
FASED at slot one. Build and focused CTest pass: 90 widget catalog bindings
and 185 atomic rejection checks. Its `--verify-ir` mode parses and verifies
the actual production circuit using CIRCT, without stock firtool annotation
lowering. A real twelve-bit AW ID connection is mutated to shared W ID;
CIRCT accepts the resulting IR, and the comparison rejects the five-channel
crosswire. The initial stock-firtool attempt instead rejected retained
FireSim annotations; that complete diagnostic is compressed in
`nic-binding-mutation/annotation-lowering-failure.log.gz`. It is not the
passing mutation evidence, which is in `nic-binding-mutation-native/`.

Evidence directory:
`/scratch/jfx/fsim-circt/sims/firesim/sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration216-u250-target-matrix`.
The three `*-binding-comparison.json` reports, their logs, `build.log`,
`binding-build.log`, `ctest.log`, `ctest-details.log` and
`nic-binding-mutation-native/` retain the focused checks. The native compiler
SHA256 is `074576676ceb555931935013ce34aa13e9ceb6c358bf85107aef32d237dc11a1`.

The next smallest porting step is the NIC host control-master binding,
comparing its full host AXI interface and dispatcher/arbiter routes with SFC
`FPGATop`/`NastiRecursiveInterconnect` before platform assembly. NIC runtime,
generalized TracerV/memory profiles, SRAM model-port differences, partitioned
QSFP semantics and the SFC UART-bearing baseline remain pending. Harness
iteration 215 passed actual CIRCT replacertl/provenance and fresh smoke;
long-suite evidence was reused only for unchanged inputs. Those results
precede this change. No manager gate was started here. The port is incomplete.

Fresh all-13 replay is complete in `iteration216-u250-target-matrix/replay/`.
Manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
All archived input hashes, effective platform configurations and prior
boundary comparison checks match iteration 215. Results remain one compiled,
six failed and six unsupported platforms; comparisons remain eight matched,
six SRAM model-port mismatches and twelve not reached. Complete per-recipe
stderr logs were inspected. Only the NIC failure advances to host-master
binding. The matrix exit 1 and rejected targets remain implementation gaps.

`artifact-regression.log` records a passing production/collateral/immutable/
matrix regression. All 226 shared NIC and 256 shared no-NIC MLIR/JSON files
match iteration 215 after normalizing only the candidate-directory annotation;
NIC adds exactly its two bound artifacts. No-NIC final RTL, both headers and
both XDC files remain byte-identical, with SV SHA256
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
Primary immutable handoff FIRRTL/annotation hashes and normalized U250 XDC
also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`,
`matrix-regression.json` and `tool-input-hashes.json` retain those checks.
Pre-existing shared edits were preserved and excluded from this checkpoint.

## Iteration 217: NIC host control-master binding

The production NIC path now invokes `bindControlMaster` after all catalog
slaves are bound. It verifies the full CIRCT module and emits
`post-fame-control-master.mlir` and `post-fame-control-master-all.json`.
The existing native composer assembles the complete host control bundle from
32 scalar AW/W/B/R boundaries and the aggregate AR boundary. The next explicit
rejection is `NIC host memory binding and platform integration are not
implemented`; NIC still does not compile to platform RTL.

The Scala oracle inspected was the InferModelPorts/FAMETransform/SimulationMapping
ordering in `MidasTransforms.scala`, clock/reset/channel handling in
`FAMETransform.scala`, `Widget.scala:HasWidgets.genCtrlIO`, and
`junctions/nasti.scala:NastiRecursiveInterconnect`. The master is connected
directly to the crossbar; sorted widget regions select the slave catalog.
The native binding adds no state and preserves the existing decoder,
dispatchers, response arbiters, tracker retirement and bridge clocks.

`NICControlMasterCompare.py` uses the shared stateless binding check extracted
from `PrintRocketControlMasterCompare.py`. The original Print comparison
retains its two allocated-Print requirements; both retained allocation orders
pass after extraction. The new NIC check also validates the twelve-region
catalog, every emitted annotation record, and a type-correct mutation of the
actual production AR bundle connection to AW.

Native build and both focused CTests pass. The control-master unit test checks
both accepted input stages, all scalar/AR routes, copied clocks, target transfer
and 26 atomic rejections. The slave-binding test passes 90 catalog bindings
and 185 atomic rejections, including the twelve-region NIC allocation.
Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration217-u250-target-matrix/`.
`build.log`, `ctest.log`, `ctest-details.log`, `print-master-comparison.json`
and `tool-input-hashes.json` retain these checks. The compiler SHA256 is
`6d37b053011d14dae4e2683b69d29aa91743ccd6e4231866820c2f84b8b19ba1`.

The next smallest porting step is the NIC FASED host-memory binding, comparing
its request/response adaptation with the SFC memory master before address
translation and platform assembly. NIC runtime, generalized TracerV/memory
profiles, SRAM model-port differences, partitioned QSFP semantics and the SFC
UART-bearing baseline remain pending. Harness iteration 216 passed actual
CIRCT replacertl/provenance and fresh Verilator smoke; unchanged-input long
suite evidence was reused. Those results precede this change. No manager gate
was started here. The port remains incomplete.

The fresh NIC composition preserves all 205 prior module bodies, 135 copied
typed ports and 1,792 emitted annotation records. It consumes exactly 33
boundary ports and produces 168 connections: 135 copies, 32 scalar channel
routes and the thirteen-leaf aggregate AR route. All shared master routes
also match the fresh no-NIC boundary. The NIC boundary SHA256 is
`92d58cd8e92758cb83b4cdfc26619f5e014892c2c22edd428e3fd197c33a87f2`.
The raw circuit annotation archive transfers exactly, including targets that
remain internal after their boundary ports are consumed.

The exact primary SFC comparison artifact is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
Its `FPGATop` control ABI agrees on all 45 widths/directions; all 24 surviving
`NastiRecursiveInterconnect` master pins have the same direct request and
response routes. B/R user outputs are zero in SFC. Native IR also retains
request fields optimized away by SFC, checked through the typed interface and
all native SSA routes. This is a complete stateless wiring check at this
boundary, not a NIC memory/platform/runtime result.

`nic-master-comparison.json` and its complete log retain these checks.
`nic-master-mutation/` contains the actual production IR with its AR aggregate
fed from the identical-typed AW aggregate. Native CIRCT parsing/verification
passes; the structured comparison rejects `actual host control SSA wiring
differs`. The retained Print comparison preserves its original profile requirements;
both original allocation orders pass with the shared helper.

The NIC boundary also passes against the same archive's
`alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`
(SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`)
and the immutable October 1 U250 build-tree fixture's
`design/FireSim-generated.sv` at
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
(SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`).
These compare their shared host ABI/routes to NIC; they do not substitute a
no-NIC golden for NIC-specific behavior. The complete reports/logs are
`no-nic-master-comparison.*`, `immutable-master-comparison.*` and
`golden-master-artifacts.json`.

The fresh all-13 archived-input replay is complete in `replay/matrix.json`.
Results remain one compiled, six failed and six unsupported platforms;
comparisons remain eight matched, six SRAM model-port mismatches and twelve
not reached. Matrix exit 1 and rejected targets remain implementation gaps.
The NIC recipe alone advances to its explicit host-memory/platform gap.

`artifact-regression.log` records the passing production/collateral/immutable/
matrix regression. All 228 shared NIC and 256 shared no-NIC MLIR/JSON files
match iteration 216 after normalizing only the candidate-directory annotation.
NIC adds exactly the two host control-master boundary files. No-NIC final
RTL, both headers and both XDC files remain byte-identical; SV SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotation hashes and normalized U250
XDC also match. All thirteen archived input hashes, effective platforms,
compile statuses and prior structured boundary comparison checks match
iteration 216; only NIC's explicit next-gap diagnostic changes. Full per-recipe
stderr was inspected. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`matrix-regression.json` retain the checks. Manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
Existing shared changes are excluded from this checkpoint.

## Iteration 218: NIC local FASED host-memory binding

The production NIC path now invokes `bindFASEDHostMemory` after host control
master assembly. The full CIRCT module is verified and the compiler emits
`post-fame-fased-host-memory.mlir` and its `-all.json` annotation archive.
The native composer converts the three split NASTI request/response bundles
into the local AXI master, preserving NIC CPU streams and all inner state.
The next explicit gap is `NIC FASED address translation and platform
integration are not implemented`; NIC does not yet compile to platform RTL.

The Scala oracle inspected was the InferModelPorts/FAMETransform/SimulationMapping
ordering in `MidasTransforms.scala`, clock/reset/channel handling in
`FAMETransform.scala`, `FASEDMemoryTimingModel.scala:298-325`, and
`junctions/nasti.scala:AXI4NastiAssigner.toAXI4Slave`. Requests connect directly
from ingress, responses to read/write egress; response-user fields are
DontCare. The native wrapper retains the 35/64/4-bit profile, all 37 AXI
leaves, two invalid response-user values and mapped leaf target identities.
Address translation, deinterleaving and platform adaptation remain separate.

`NICFASEDHostMemoryCompare.py` checks the actual production SSA, all typed
copied ports, all prior module bodies, both annotation representations and
the twelve-region allocation including SimpleNIC. It uses the stateless
binding check extracted from `PrintRocketFASEDHostMemoryCompare.py`; the
original Print comparison retains its two allocation orders and identity
probe requirements. Both retained Print orders pass after extraction.
The NIC check includes a type-correct mutation that drives the actual local
AR address from AW, followed by native CIRCT verification and rejection by
the structured comparison.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration218-u250-target-matrix/`.
The native compiler build and focused `goldengate-fased-host-memory` CTest
pass: 37 AXI leaves, two invalid users, target transfer and twelve atomic
rejections. `build.log`, `ctest.log`, `ctest-details.log`,
`print-memory-comparison.json` and `tool-input-hashes.json` retain these
checks. Compiler SHA256:
`577a678404a387b0a28cf4b4c31ed5b136c3f74e6c6adc7b0b080c2c05039388`.

The exact NIC SFC artifact compared is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
The shared local boundary also matches the archive's
`alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`
(SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`)
and the immutable October 1 U250 build-tree fixture's
`design/FireSim-generated.sv` at
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
(SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`).
All three agree on 35 surviving local FASED AXI widths/directions and direct
ingress/egress/error routes. Two response-ready leaves optimized away by
SFC are checked through the native typed interface and actual SSA instead.
`golden-memory-artifacts.json` records the exact ports, routes and hashes.
These shared-boundary comparisons do not establish NIC platform/runtime
parity or substitute a no-NIC golden for NIC-specific behavior.

The next smallest porting step is NIC FASED address translation, checking
its offset arithmetic, bounds assertions and clocks against SFC before
advancing to read deinterleaving. Generalized TracerV/memory profiles, SRAM
model-port differences, partitioned QSFP semantics, NIC runtime and the SFC
UART-bearing baseline remain pending. Harness iteration 217 passed actual
CIRCT replacertl/provenance and fresh Verilator smoke; unchanged-input long
suite evidence was reused. Those results precede this change. No manager
gate was started here. The port remains incomplete.

The fresh NIC boundary comparison passes against all three SFC artifacts,
including a shared-route check with this iteration's fresh no-NIC candidate.
All 206 prior module bodies, 133 copied typed ports (including 26 CPU stream
ports), 1,792 emitted annotations and the raw circuit archive are preserved.
The wrapper has 172 connections: 133 copies and 39 local memory routes,
including two invalid response users. Boundary SHA256:
`41930599d7882db33fa2cc9804095e4478b8853a830dcfbdd8ccdf95cefded99`.
`nic-memory-comparison.*`, `no-nic-memory-comparison.*` and
`immutable-memory-comparison.*` retain the complete results.
`nic-memory-mutation/` contains the actual production circuit with the AR
address fed from the same-typed AW address. Native CIRCT parsing and
verification pass; the structured comparison rejects `actual host memory
SSA wiring differs`. This check supplements the exact stateless wiring
comparison; it does not claim translation or NIC runtime coverage.

The fresh all-thirteen archived-input replay is complete in
`replay/matrix.json`. Results remain one compiled, six failed and six
unsupported platforms; prior boundary comparisons remain eight matched,
six SRAM model-port mismatches and twelve not reached. Replay exit 1 and
rejected targets remain implementation gaps. All archived input hashes,
effective platform values, compile statuses and existing structured checks
match iteration 217; only NIC's next-gap diagnostic changes. Full per-recipe
stderr was inspected in `artifact-regression.log`.

The production/collateral/immutable/matrix regression passes. All 230 shared
NIC and 256 shared no-NIC MLIR/JSON files match iteration 217 after normalizing
only the candidate-directory annotation; NIC adds exactly the two local
host-memory boundary files. No-NIC final RTL, both headers and both XDC files
are byte-identical to iteration 217; RTL SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotation hashes and normalized U250
XDC also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`matrix-regression.json` retain the checks. Manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.
Pre-existing shared edits are preserved and excluded from this checkpoint.

## Iteration 219: NIC local FASED address translation

The production NIC path now invokes `addFASEDAddressTranslation` after local
host-memory binding. It verifies the full CIRCT module and emits
`post-fame-fased-address-translation.mlir` plus its `-all.json` annotation
archive. The existing native composer implements the recorded MainMemory_0
allocation: virtual addresses start at 0x80000000, end at 0x47fffffff, and
translate into a 34-bit host region at zero. NIC still stops explicitly at
`NIC FASED read deinterleaving and platform integration are not implemented`.
This change does not claim complete NIC compilation or runtime parity.

The Scala sources inspected were the InferModelPorts/FAMETransform/SimulationMapping
ordering in `MidasTransforms.scala`, clock/reset handling in `FAMETransform.scala`,
`widgets/AXI4AddressTranslation.scala` and region allocation in `core/FPGATop.scala`.
AW and AR add the wrapped offset independently; all other AXI leaves pass
through. Four host-clock assertions check valid requests against the virtual
bounds independently of ready, with reset disabling the checks. The native
wrapper preserves CPU streams, clocks, prior state and pre-translation
memory target identity while transferring copied boundary targets outward.

`NICFASEDAddressTranslationCompare.py` checks actual production SSA, typed
ports, all prior module bodies, emitted annotations and the raw circuit
archive. It reuses the generic binding comparison extracted from
`PrintRocketFASEDAddressTranslationCompare.py`; the original Print comparison
retains both allocation orders, exact module/port counts and seven identity
probes. The native helper is evaluated over 1,024 address, valid, ready and
reset combinations. A type-correct mutation changes the actual offset by
one; native CIRCT verification must accept the circuit and the structured
comparison must reject the wrong arithmetic.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration219-u250-target-matrix/`.
The exact NIC SFC artifact is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
Its local `AXI4AddressTranslation` agrees with the archive's
`alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`
(SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`)
and the immutable October 1 U250 build-tree fixture's
`design/FireSim-generated.sv` at
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
(SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`).
All three have the same 74 surviving port widths/directions, 36 channel
routes (including both address translations), seven predicate equations,
four ordered assertions and host-clock/reset behavior. SFC removes both
B.ready ports; the native comparison checks that preserved ready route.
`golden-translation-artifacts.json` retains the exact contracts and hashes.
These shared local comparisons do not replace NIC-specific integration tests.

The next smallest porting step is NIC FASED read deinterleaving, comparing
its per-ID queues, AXI ordering and backpressure against NIC SFC before
advancing further. Generalized TracerV/memory profiles, SRAM model-port
differences, partitioned QSFP semantics, NIC runtime and the SFC UART-bearing
baseline remain pending. Harness iteration 218 passed actual CIRCT
replacertl/provenance and fresh Verilator smoke, with unchanged-input long
suite evidence reused. Those gates precede this change; no manager gate was
started here. Pre-existing shared edits are preserved and excluded from this
checkpoint. The overall port remains incomplete.

The native build and focused `goldengate-fased-address-translation` CTest
pass: 35 pass-through leaves, two translated addresses, four assertions,
1,024 semantic vectors and fifteen atomic rejections. `build.log`,
`ctest.log` and `print-translation-comparison.json` retain native and both
Print-order results. Compiler SHA256:
`6481c01e3848ac8f0546e079aee413f7191c2fc7bc44ce0c968670bd9941acb3`.
`tool-input-hashes.json` records compiler/verifier/tool and manifest hashes.

The fresh production NIC translation comparison passes against all three
SFC contracts and shares the exact helper with the fresh no-NIC candidate.
All 207 prior module bodies, 133 copied typed ports (including 26 CPU stream
ports), 1,792 emitted annotations and the raw annotation archive are
preserved. The wrapper has 137 connections, including the four clock,
reset and translated-memory bundle connections. Boundary SHA256:
`54f03893a83d09254001ed6fc1c03f22f6393f8bcb40afc34e2a089c4345624e`.
`nic-translation-comparison.json`, `no-nic-translation-comparison.json`,
`immutable-translation-comparison.json`, `translation-comparisons.log` and
`nic-translation-verify.log` retain the results. The no-NIC report here
compares NIC against the no-NIC SFC contract; the separate fresh no-NIC
wrapper check is retained in `fresh-no-nic-translation-comparison.json`.
`nic-translation-mutation/` retains the full production circuit with the
35-bit offset constant increased by one. Native CIRCT parsing/verification
pass; the structured comparison rejects `actual address translation SSA
routes differ`. `nic-translation-mutation.json` records the mutant hash.
These checks cover local stateless translation, not NIC runtime parity.

The fresh all-thirteen archived-input replay completes in `replay/matrix.json`.
Results remain one compiled, six failed and six unsupported platforms; the
existing bridge/channel and model-port comparisons remain eight matched,
six SRAM model-port mismatches and twelve not reached. Rejected targets
remain implementation gaps. All archived input hashes, effective platforms,
compile statuses and existing structured checks match iteration 218; only
NIC's next-gap diagnostic changes. All per-recipe stderr was inspected.

The production/collateral/immutable/matrix regression passes. All 232 shared
NIC and 256 shared no-NIC MLIR/JSON files match iteration 218 after normalizing
only the candidate-directory annotation. NIC adds exactly the two translation
boundary files. No-NIC final RTL, both headers and both XDC files remain
byte-identical to iteration 218; RTL SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
Primary immutable handoff FIRRTL/annotation hashes and normalized U250 XDC
also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json`,
`matrix-regression.json` and `artifact-regression.log` retain the checks.
The new comparison CLI also passes (`nic-translation-cli.json` and `.log`).
Manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.

## Iteration 220: NIC local FASED response deinterleaving

The production NIC path now invokes `addFASEDReadDeinterleaver` after address
translation, verifies the full CIRCT module and emits
`post-fame-fased-read-deinterleaver.mlir` and `-all.json`. The native pass
inserts sixteen eight-beat response queues between FASED and translation.
It preserves all existing module bodies except the internal memory connection
in `GGFASEDAddressTranslationWrapper`, and retains its ports and annotations.
NIC still stops explicitly at `NIC FASED host memory buffering and platform
integration are not implemented`; this is a partial compiler boundary.

The Scala oracle inspected was the Golden Gate/FAME ordering in
`passes/MidasTransforms.scala`, channel firing and host clock/reset handling
in `passes/fame/FAMETransform.scala`, the memory chain in `core/FPGATop.scala`
and `rocket-chip/src/main/scala/amba/axi4/Deinterleaver.scala`. AW, W, B and
AR pass through. R responses demultiplex into per-ID queues; completed-burst
counts include simultaneous accepted enqueue/retirement before choosing the
lowest pending ID. A selected burst remains locked through backpressure
until accepted RLAST. Reset flushes pointers/counts/lock, while RAM writes
retain the recorded Queue semantics.

`NICFASEDReadDeinterleaverCompare.py` compares the actual production boundary,
emitted annotation archive, copied CPU streams and NIC MMIO allocation. It
executes the composed native helper/queues through the existing C++ SSA
interpreter against an independent burst model. Its negative test changes
the actual queue enqueue-pointer increment from one to zero: native CIRCT
verification must still pass, while both behavior and structured comparisons
must reject it. `PrintRocketFASEDReadDeinterleaverCompare.py` supplies the
shared binding check; the original Print-specific counts and both allocation
orders remain checked.

The exact SFC NIC artifact is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
Its `AXI4Deinterleaver` instantiates `Queue_36`, while the no-NIC archive and
immutable October 1 U250 `design/FireSim-generated.sv` instantiate `Queue_34`.
All three pass the same checks: 73 surviving typed ports, 30 pass-through
routes, sixteen eight-entry queues with 71-bit payloads, four-bit completed
burst counts, 32 bounds assertions, next-count arbitration and burst locking.
The comparison explicitly accepts the queue module's supplied generated name.
SFC eliminates B.ready on both sides and input R.ready; native retains and
behaviorally tests response backpressure.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration220-u250-target-matrix/`.
`golden-deinterleaver-artifacts.json` records all three exact SFC paths,
hashes and contracts. `print-deinterleaver-comparison.json` records both
retained Print-order comparisons. These shared local contracts do not prove
NIC platform integration or runtime parity.

The native build and focused `goldengate-fased-read-deinterleaver` CTest pass.
It checks 2,048 queue state samples (reset writes, full queues, wraparound and
RAM collisions), 8,364 hierarchical SSA cycles (6,050 accepted input and
3,927 output beats, 1,945 stalls, 186 full-queue observations, 721 simultaneous
last-beat retirements/enqueues and 42 resets), and eleven atomic rejection
cases. `build.log` and `ctest.log` retain those results. Compiler SHA256 is
`f2fe0ea7faaa7ee8f5c87a9e6dfdc93ee032a6f187e1d9212c7e15dc87316958`;
`tool-input-hashes.json` records compiler, verifier, behavioral-test executable,
firtool, Verilator and golden-manifest hashes.

The fresh production NIC comparison passes against all three SFC contracts
and matches the fresh no-NIC helper and queue bodies exactly. It preserves
208 prior module bodies, 134 typed ports, 26 CPU-stream ports and all 1,792
emitted annotations, including the raw circuit archive. The wrapper has
140 connections after inserting clock/reset and memory wiring. Boundary
SHA256 is `0794dbeb6033e4171dcf0354388d4d6dc91932b6364d4cca71aab03a5385144b`.
`nic-deinterleaver-comparison.json`, `no-nic-deinterleaver-comparison.json`
and `immutable-deinterleaver-comparison.json` retain the NIC boundary checks
against each SFC fixture; `fresh-no-nic-deinterleaver-comparison.json`
separately checks the no-NIC production wrapper. Both original Print orders
also pass against this fresh no-NIC helper.

`nic-deinterleaver-cli.json` and `.log` retain the comparison CLI result.
Its `.behavior.log` records execution of the actual production NIC helper
and queues with the same coverage as the CTest. The pointer mutation in
`nic-deinterleaver-mutation/` passes native CIRCT parsing/verification but
fails actual SSA evaluation at `Queue_34 pointer/full next state` and the
structured check at `shared semantics changed: GGFASEDDeinterleaveQueue8`.
(The C++ diagnostic retains the original SFC queue's name.) The mutated full
production boundary has SHA256
`cd0e5297482f145e79c69aa6bd82ffd905296e7bfbefc83540dd16b6a03fc2b8`.

The next smallest porting step is NIC FASED host-memory buffering: compare
all five AXI channel queues after translation against the actual NIC SFC
buffer before advancing to host-memory arbitration. Broader TracerV/memory
profiles, SRAM model-port differences, partitioned QSFP semantics, NIC runtime
and the SFC UART-bearing baseline remain pending. Harness iteration 219
passed actual CIRCT replacertl/provenance and fresh Verilator smoke with
unchanged-input long-suite evidence reused. Those results precede this
change; no manager gate was started here. Pre-existing shared edits are
preserved and excluded from this checkpoint. The overall port is incomplete.

The fresh all-thirteen archived-input replay completes in `replay/matrix.json`.
Results remain one compiled, six failed and six unsupported platforms. Existing
annotation comparisons remain eight matched, six SRAM model-port mismatches
and twelve not reached. Rejections are implementation gaps. Input hashes,
effective platform configurations, compile statuses and existing boundary
checks match iteration 219; only NIC's next-gap diagnostic changes. Complete
per-recipe stderr was inspected and is retained in `matrix-regression.log`.
`matrix-regression.json` records the aggregate checks; the matrix exits one
because the gaps remain, rather than reporting partial comparisons as parity.

The artifact regression passes: all 234 shared NIC and 256 shared no-NIC
MLIR/JSON files match iteration 219 after normalizing only the candidate
output-directory annotation. NIC adds exactly the two deinterleaver files.
No-NIC final RTL, both headers and both XDC files remain byte-identical;
RTL SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotation hashes and normalized U250
XDC also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`artifact-regression.log` retain the results. Golden-manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.

## Iteration 221: NIC FASED host-memory buffering

The production NIC path now invokes `addFASEDHostMemoryBuffer`, verifies the
full CIRCT module and emits `post-fame-fased-host-memory-buffer.mlir` and
`-all.json`. The pass inserts five independent depth-two, no-flow/no-pipe
queues between translated FASED memory and the external memory boundary.
AW and AR share a queue definition, with separate instance state. NIC stops
explicitly before shared host-memory arbitration at `NIC host memory write
arbitration and platform integration are not implemented`.

The Scala oracle inspected was Golden Gate ordering in
`passes/MidasTransforms.scala`, channel firing and host clock/reset handling
in `passes/fame/FAMETransform.scala`, and the memory chain in
`core/FPGATop.scala`: FASED, deinterleaver, address translation, AXI4Buffer,
then the shared memory crossbar. Queue occupancy resets synchronously;
accepted RAM writes retain their reset-independent behavior. Async reads,
backpressure, full/empty transitions and the five channel directions are
preserved by CIRCT/MLIR operations.

`NICFASEDHostMemoryBufferCompare.py` checks the actual production boundary,
annotation archive, NIC MMIO region and CPU stream interfaces, then executes
the generated queue/helper SSA with the native C++ interpreter. A negative
test replaces the actual enqueue-pointer toggle with a type-correct hold;
full CIRCT verification must accept the mutant, while behavior and structured
comparison must reject it. `PrintRocketFASEDHostMemoryBufferCompare.py`
provides the shared check and retains the original Print-specific counts.
Its SFC contract now also checks the 36 surviving translation-to-buffer
connections, host clock/reset and always-enabled asynchronous RAM reads.

The exact NIC SFC artifact compared is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
`AXI4Buffer_1` instantiates `Queue_31` for AW/AR, `Queue_25` for W,
`Queue_33` for B and `Queue_35` for R. The no-NIC archive and immutable
October 1 U250 `design/FireSim-generated.sv` use `Queue_29/23/31/33`.
All three match 75 surviving typed ports, 54 payload connections, the same
36 translated-memory routes, depth-two queue control and RAM semantics.
The four queue payload widths are 63, 73, 6 and 71 bits. SFC removes B.ready
at the dequeue boundary; native retains and behaviorally tests its stalls.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration221-u250-target-matrix/`.
`golden-buffer-artifacts.json` records exact SFC paths, hashes and contracts;
`tool-input-hashes.json` records the compiler, native verifier, behavioral
test, firtool, Verilator and archived manifest. `build.log` retains the native
build. These local contracts do not establish NIC platform/runtime parity.

The focused `goldengate-fased-host-memory-buffer` CTest passes: 256 exhaustive
queue states (48 reset writes, 64 full states and 32 collisions), 6,000
five-channel hierarchical SSA edges (17,380 accepted inputs, 17,147 outputs,
8,350 stalls, 6,765 full observations, 9,487 simultaneous transfers and 44
resets), and seven atomic rejection cases. `ctest.log` retains these results.
Compiler SHA256 is
`5635be6081b0189633efc77d8af631f8704328cee5bd97b867a2752fa5d66cf1`.

The next smallest porting step is NIC shared host-memory AW/W arbitration:
compare the actual SFC crossbar's source reservation, AW backpressure,
accepted-WLAST retirement and LoadMem/FASED ID allocation before advancing
that boundary. Remaining risks include NIC platform/runtime integration,
broader TracerV/memory profiles, SRAM model-port differences, partitioned
QSFP semantics and the pending SFC UART-bearing baseline. Harness iteration
220 passed CIRCT replacertl/provenance and fresh Verilator smoke; unchanged
long-suite evidence was reused. Those gates precede this change. No manager
gate was started here, and unrelated shared edits remain outside this change.
The overall port remains incomplete.

The fresh production NIC boundary matches all three SFC contracts and the
fresh no-NIC queue/helper bodies. It preserves 210 existing module bodies,
134 typed ports, 26 CPU-stream ports, twelve control regions and all 1,792
emitted annotations, including the raw archive. The wrapper has 143
connections after insertion; the helper has 84 exact connections. Boundary
SHA256 is `c2de210dc6a6dcfe1fa100afb89f0c5726c1049aab67c09b05f9879e0ae1f724`.
`nic-buffer-comparison.json`, `no-nic-buffer-comparison.json` and
`immutable-buffer-comparison.json` record each SFC comparison.
`fresh-no-nic-buffer-comparison.json` checks the production no-NIC wrapper;
`print-buffer-comparison.json` checks both retained Print allocation orders
against the fresh no-NIC helper.

`nic-buffer-cli.json`, `.log` and `.behavior.log` record the passing new CLI
and actual production SSA execution, with the same state/traffic coverage as
the focused CTest. The pointer-hold mutant in `nic-buffer-mutation/` passes
full native parsing/verification but fails behavior at
`occupancy reset/next state mismatch` and structured comparison at
`shared semantics changed: GGFASEDMemoryAddressQueue2`. Mutant SHA256 is
`016cc9f64ec47920bc27952bb3625954d654c9e57161c29720124ad83f9ce169`.

The artifact regression passes: all 236 shared NIC and 256 shared no-NIC
MLIR/JSON files match iteration 220 after normalizing only the candidate
output-directory annotation. NIC adds exactly the two buffer boundary files.
No-NIC final RTL, both headers and both XDC files remain byte-identical;
RTL SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotation hashes and normalized U250
XDC also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`artifact-regression.log` retain these checks. Golden-manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.

The fresh all-thirteen replay completes in `replay/matrix.json`: one compiled,
six failed and six unsupported platforms. Existing annotation comparisons
remain eight matched, six SRAM model-port mismatches and twelve not reached.
Rejected recipes remain implementation gaps. Archived input hashes, effective
platform settings, statuses and earlier boundary checks match iteration 220;
only NIC's next-gap diagnostic changes. Complete per-recipe stderr was read
and retained in `matrix-regression.log`; `matrix-regression.json` records the
aggregate checks. The matrix exits one because these gaps remain. No full
NIC compilation, emitted platform RTL or runtime parity is claimed.

## Iteration 222: NIC shared host-memory AW/W arbitration

The production NIC path now invokes `addHostMemoryWriteArbiter`, verifies
the full CIRCT module and emits `post-fame-host-memory-write-arbiter.mlir`
and `-all.json`. The pass creates the two-source AW/W crossbar and its
depth-two flow source queue using FIRRTL operations. AW reserves a source
before downstream acceptance; that reservation persists through AW stalls
and retires only on an accepted WLAST. LoadMem uses ID 16 and FASED IDs
remain 0 through 15, extended to the five-bit shared interface. NIC stops
explicitly at `NIC host memory write responses and platform integration
are not implemented` before B routing.

The Scala oracle inspected was Golden Gate ordering in
`passes/MidasTransforms.scala`, channel firing and host clock/reset handling
in `passes/fame/FAMETransform.scala`, and the shared memory crossbar in
`core/FPGATop.scala`. The existing native implementation preserves the
buffered FASED source and forwards other interfaces, including NIC CPU
streams, through the new wrapper.

`NICHostMemoryWriteArbiterCompare.py` checks the actual production wrapper,
all retained module bodies, shared helpers, CPU stream and MMIO interfaces,
and raw/emitted annotation target transfer. It executes the generated
arbiter/queue SSA with the native C++ interpreter. Its negative test changes
the actual source queue's enqueue-pointer toggle into a type-correct hold;
full native verification must accept the mutant while behavior and
structured comparison must reject it.
`PrintRocketHostMemoryWriteArbiterCompare.py` supplies the generalized binding
check while retaining the original Print-specific counts. Its SFC contract
now checks the instantiated source queue, all AW/W payload muxes and five
arbitration state updates, alongside reservation, backpressure and WLAST
retirement equations.

The exact NIC SFC artifact compared is
`automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`
(SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`).
Its `AXI4Xbar.awOut_0` instantiates `Queue_23`; the archived no-NIC and
immutable October 1 U250 `design/FireSim-generated.sv` use `Queue_21`.
All three match 73 combinational crossbar equations, five arbitration state
updates, eighteen queue equations and three queue state updates. The queue is depth two, flow enabled,
pipe disabled, with two-bit source payloads and reset-independent RAM writes.
The shared interface has 34-bit addresses, 64-bit data and five-bit IDs.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration222-u250-target-matrix/`.
`golden-arbiter-artifacts.json` records exact golden paths, hashes and
contracts. `tool-input-hashes.json` records the native compiler, verifier,
behavioral test, firtool, Verilator and archived manifest; `build.log` retains
the successful native build. Compiler SHA256 is
`771081c7d474da8bc617e8f32def544e26eff53f7b013fee813d5f7cb56c785b`.

The focused `goldengate-host-memory-write-arbiter` CTest passes: 256
exhaustive queue cases (40 reset RAM writes and sixteen bypasses), 216,018
leaf checks over 12,001 hierarchical SSA edges, and nine atomic rejection
cases with retained target identities. Traffic coverage includes 9,988 full
observations, 7,975 stalls, 464 pre-acceptance reservations, fourteen bypasses,
162 simultaneous enqueue/dequeue edges and 52 resets. `ctest.log` retains
the results.

The fresh production NIC boundary passes all three SFC contracts and matches
the fresh no-NIC arbiter/queue helper bodies. It preserves 216 existing
module bodies, 26 CPU-stream ports, twelve control regions and all 1,792
emitted annotation records with the expected circuit/public-target transfer.
The new wrapper has 125 copied typed ports, 127 total ports and 141 exact
connections. Boundary SHA256 is
`d559867aecc989350451ebff0b2a5468150c7349b5baa8855bbb17fd2aeea607`.
`nic-arbiter-comparison.json`, `no-nic-arbiter-comparison.json` and
`immutable-arbiter-comparison.json` record each golden comparison;
`fresh-no-nic-arbiter-comparison.json` checks the production no-NIC wrapper.
`print-arbiter-comparison.json` checks both retained Print allocation orders:
272 unchanged module bodies, 116 copied ports, 118 total ports, 132 wrapper
connections and all 21 Print modules per order.

`nic-arbiter-cli.json`, `.log` and `.behavior.log` record the passing new CLI
and execution of the actual production NIC SSA with the same queue/traffic
coverage as the focused CTest. The pointer-hold mutant in
`nic-arbiter-mutation/` passes native parsing/verification, fails behavior at
`queue next enq` and fails structured comparison at
`shared helper differs GGHostMemoryWriteSourceQueue2`. Mutant SHA256 is
`ce21e3f3a955d4edbe77f125ceca6ca4832229f671b826a81bb1ab962c935870`.

The artifact regression passes: all 238 shared NIC and 256 shared no-NIC
MLIR/JSON files match iteration 221 after normalizing only the candidate
output-directory annotation. NIC adds exactly the two AW/W boundary files.
No-NIC final RTL, both headers and both XDC files remain byte-identical;
RTL SHA256 remains
`866b0e02cc633b969f0945f987b5577ecd6bb578301cb7a1e6aba6eb056a60da`.
The primary immutable handoff FIRRTL/annotation hashes and normalized U250
XDC also match. `production-boundary-regression.json`,
`no-nic-emitted-regression.json`, `immutable-rocket-comparison.json` and
`artifact-regression.log` retain these results. Golden-manifest SHA256 remains
`0292a691278b4c59514b2675bee30af306a382caac33cc94978aa5c590590ffe`.

The next smallest porting step is NIC host-memory B-response routing:
compare the SFC crossbar's ID 16 LoadMem selection, four-bit FASED ID
truncation, invalid-ID blocking and sink-specific backpressure before
advancing that boundary. Remaining risks include NIC AR/R and platform/runtime
integration, broader TracerV/memory profiles, SRAM model-port differences,
partitioned QSFP semantics and the pending SFC UART-bearing baseline.
Harness iteration 221 passed CIRCT replacertl/provenance and fresh Verilator
smoke; unchanged long-suite evidence was reused. Those gates precede this
change. No manager gate was started here, and unrelated shared edits remain
outside this change. The overall port remains incomplete.

The fresh all-thirteen replay completes in `replay/matrix.json`: one compiled,
six failed and six unsupported platforms. Earlier annotation comparisons
remain eight matched, six SRAM model-port mismatches and twelve not reached.
Archived input hashes, effective platform configurations, compile statuses
and existing boundary checks match iteration 221; only NIC's next-gap
diagnostic changes. Complete per-recipe stderr was inspected and is retained
in `matrix-regression.log`; `matrix-regression.json` records the aggregate
checks. The matrix exits one because these gaps remain. These local AW/W
checks do not establish full NIC compilation, emitted platform RTL or runtime
parity.

## Iteration 223: NIC host-memory B-response routing

The production NIC path now invokes `routeHostMemoryWriteResponses`, verifies
the CIRCT module and emits `post-fame-host-memory-write-responses.mlir` and
`-all.json`. The pass builds a combinational FIRRTL response demultiplexer:
LoadMem selects ID 16; FASED selects IDs 0 through 15 and receives their low
four bits; IDs 17 through 31 produce neither valid nor ready. Host ready is
independent of host valid and follows only the selected sink's backpressure.
AW/W and the remaining AR/R channels are forwarded through the new wrapper.
NIC stops explicitly at `NIC host memory read arbitration and platform
integration are not implemented` before the AR arbiter.

The Scala oracle inspected was Golden Gate pass ordering in
`passes/MidasTransforms.scala`, host-domain and channel firing semantics in
`passes/fame/FAMETransform.scala`, the shared host-memory crossbar in
`core/FPGATop.scala`, and ID ranges/response fanout in Rocket Chip's
`amba/axi4/Xbar.scala`. The native pass retains the prior buffered masters,
reservation queue and unrelated interfaces, including NIC CPU streams.

`NICHostMemoryWriteResponsesCompare.py` compares production bindings, typed
ports, all retained module bodies, raw and emitted annotation target transfer,
CPU stream ports and MMIO regions. It executes the emitted router SSA with
the native C++ interpreter. Its negative test changes the actual LoadMem
selector from ID 16 to ID 17 with unchanged FIRRTL types: native verification
must accept it while both behavior and structured comparison reject it.
`PrintRocketHostMemoryWriteResponsesCompare.py` now exposes a general binding
comparison while retaining the Print-specific counts; its SFC contract checks
all ten B-channel port types as well as the seven response equations.

The native build and focused `goldengate-host-memory-write-responses` CTest
pass. The test executes 1,024 exhaustive ID/response/valid/sink-ready cases,
5,120 leaf checks and 480 unmatched-ID cases. It checks eight boundary routes,
fourteen atomic rejections, seven target identities and retained inner bodies.
Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration223-u250-target-matrix/`.
`build.log`, `ctest.log` and `tool-input-hashes.json` record the build, focused
test and exact tool inputs. Native compiler SHA256 is
`da8232f1e84288959840c7df3cd44c7f332adb106a660e20fc81c38d1f58e507`.

Fresh production NIC comparison passes: 219 unchanged prior module bodies,
123 copied ports, 125 wrapper ports, 131 connections and 1,792 annotation
records. All 26 CPU-stream ports and twelve MMIO regions remain; SimpleNIC
retains slave 7 at byte address 512 with size 32. The emitted B helper is
identical to fresh no-NIC Rocket's helper. The actual NIC boundary SHA256 is
`ac856185ea07e0b4ce283a2d7961fa0dc1ba4391451f465f6df98e8474391e21`.
`nic-response-cli.json` records these checks and native execution of its
1,024 response cases and eight boundary bindings. The ID-17 mutation passes
CIRCT verification but fails actual SSA execution with `AXI4Xbar B cone
mismatch` and structural comparison with `shared B router differs`.

The seven B equations and ten typed ports match `AXI4Xbar` in all three
recorded SFC artifacts:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.

The corresponding reports are `nic-response-comparison.json`,
`no-nic-response-comparison.json`, `immutable-response-comparison.json` and
`golden-response-artifacts.json`. Fresh no-NIC binding comparison also passes
(205 retained bodies, 118 copied ports), as do both retained Print orders
(275 retained bodies, 114 copied ports and 21 retained Print modules).
`production-boundary-regression.json` confirms all 240 earlier NIC and 256
no-NIC MLIR/JSON files are unchanged, apart from annotation output-directory
normalization; NIC adds only the two B boundary artifacts. All five no-NIC
emitted RTL/header/XDC files are byte-identical to iteration 222.

`immutable-rocket-comparison.json` confirms archived no-NIC FIRRTL and
annotations match the immutable staging handoff. Its synthesis XDC matches
the immutable U250 build tree exactly; implementation XDC matches after the
recorded `target/FireSim_` to `target_FAMETop/FireSim` hierarchy substitution.
These comparisons do not establish full NIC RTL or runtime parity.

The fresh full thirteen-recipe replay finishes with one compiled target,
six compiler gaps and six unsupported partition/QSFP platforms. All archived
FIRRTL/annotation hashes and effective platform selections match iteration
222. All seven reached bridge/channel comparisons match. Saturn Shuttle's
model-port comparison matches; the other six reached model-port comparisons
retain their native-SRAM differences and require separate equivalence checks.
The aggregate comparison counts are eight matched, six mismatched and twelve
not reached. Full diagnostics are in `matrix-regression.log`, with structured
results in `replay/matrix.json` and `matrix-regression.json`.

The other compiler gaps remain multiple or non-Rocket TracerV constructors
(quad-core Rocket, BOOM and Saturn Shuttle), and FASED outstanding profiles
other than maxFlight=10 (Gemmini and Saturn Rocket). The replay exits one
because the gaps remain; it is not an all-target success. The next smallest
NIC porting step is to compose the CIRCT host-memory AR arbiter, preserving
CPU-stream ports and annotation identities, and compare its arbitration and
ID mapping against the same SFC crossbar before advancing the R path.
Manager replacertl, Verilator metasimulation and U250 buildbitstream remain
owned by the external verification harness; this iteration launches none.

## Iteration 224: NIC host-memory AR arbitration

The production NIC path now invokes `addHostMemoryReadArbiter`, verifies the
resulting CIRCT FIRRTL operations and emits
`post-fame-host-memory-read-arbiter.mlir` and `-all.json`. The native pass
arbitrates LoadMem and FASED read addresses with synchronous host-clock state.
It holds the selected source during stalls, rotates round-robin priority on
idle selection, inserts LoadMem ID 16 and zero-extends FASED's four-bit IDs.
LoadMem uses length zero, size three and incrementing burst. The wrapper
retains AW/W/B, the pending R channels and unrelated interfaces. NIC stops at
`NIC host memory read responses and platform integration are not implemented`
before the R response pass; this is not full NIC compilation.

The Scala oracle inspected was Golden Gate ordering in
`passes/MidasTransforms.scala`, host clock/reset and channel firing in
`passes/fame/FAMETransform.scala`, the crossbar construction in
`core/FPGATop.scala`, Rocket Chip's `AXI4Arbiter.returnWinner` in
`amba/axi4/Xbar.scala` and `TLArbiter.roundRobin` in `tilelink/Arbiter.scala`.
The selection mask updates on idle selection even when the sink is stalled;
input ready follows the allowed source rather than being gated by valid.

`PrintRocketHostMemoryReadArbiterCompare.py` now exposes its binding comparison
for production boundaries while retaining the historical Print-specific
counts. Its SFC contract covers all 25 AR port types, 49 equations and seven
state-update anchors. `NICHostMemoryReadArbiterCompare.py` checks actual
production bindings, retained module bodies, copied CPU-stream ports, MMIO
regions, recursive raw annotations and emitted annotation records. It runs
the native C++ SSA interpreter on the emitted production helper. Two negative
cases mutate that actual helper: LoadMem ID 16 becomes 17, and the LoadMem
selection recomputes during stalls. Both mutations must pass native FIRRTL
verification but fail behavior and structured comparison.

Native compilation and the focused `goldengate-host-memory-read-arbiter`
CTest pass. The native test executes 512 exhaustive state/input/reset cases
(8,192 checks) and a 12,001-cycle trace (192,016 checks), including 5,056 stalled
cycles and 5,143 contention cycles. It also checks eleven boundary routes,
eleven atomic contract rejections, eight target identities and unchanged
inner bodies. Local evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration224-u250-target-matrix/`.
`build.log`, `ctest.log` and `tool-input-hashes.json` record the tools and
focused test; native compiler SHA256 is
`471894e3f7914c497ca766652d6a0470ccb02e2a5106f7588158d8fb678d3818`.

Production NIC comparison passes with 221 unchanged prior module bodies,
120 copied ports, 122 wrapper ports and 131 connections. All 1,792 annotation
records, 26 CPU-stream ports and twelve MMIO regions are retained; SimpleNIC
remains slave 7 at byte address 512, size 32. The helper matches the fresh
no-NIC implementation exactly. The actual emitted NIC AR boundary SHA256 is
`cf498aaa0af6d953b8e259305410848686ed65b930700739805469f93ae725e5`.
`nic-ar-cli.json` and its `.behavior.log` record native execution of the
production SSA through the same 512 exhaustive cases and 12,001-cycle trace.
Both mutation artifacts pass native verification, fail behavior with
`AR payload mismatch` and fail comparison with `shared AR arbiter differs`.

The AR contract matches `AXI4Xbar` in all three recorded SFC artifacts:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.

`nic-ar-comparison.json`, `no-nic-ar-comparison.json`,
`immutable-ar-comparison.json` and `golden-arbiter-artifacts.json` retain these
comparisons. Fresh no-NIC composition also passes (207 retained bodies,
115 copied ports, 117 top ports and 126 connections), as do both historical
Print orders (277 retained bodies, 111 copied ports, 113 top ports,
122 connections and 21 retained Print modules).

`production-boundary-regression.json` confirms all 242 earlier NIC and 256
no-NIC MLIR/JSON files match iteration 223 after output-directory normalization;
NIC adds only the two AR boundary artifacts. `no-nic-emitted-regression.json`
confirms all five emitted RTL/header/XDC files are byte-identical to iteration
223. `immutable-rocket-comparison.json` rechecks that archived no-NIC FIRRTL and
annotations match the immutable staging handoff. Synthesis XDC matches the
immutable U250 build-tree reference exactly; implementation XDC matches after
the recorded `target/FireSim_` to `target_FAMETop/FireSim` hierarchy substitution.
These are local composition and regression comparisons, not proof of final
NIC platform RTL or runtime parity.

The fresh thirteen-recipe replay completes with one compiled target, six
compiler gaps and six unsupported partition/QSFP platforms. All archived
input hashes, effective platform configurations, compile statuses and earlier
boundary checks match iteration 223; only NIC's next-gap diagnostic changes.
All seven reached bridge/channel comparisons match. Saturn Shuttle's
model-port comparison matches; the other six reached model-port comparisons
retain native-SRAM differences requiring separate equivalence checks. The
aggregate is eight matched, six mismatched and twelve not reached.
`replay/matrix.json`, `matrix-regression.json` and the complete per-recipe
stderr in `matrix-regression.log` record these outcomes. The replay exits one
because the implementation gaps remain.

Remaining compiler gaps include multiple/non-Rocket TracerV constructors,
FASED outstanding profiles other than maxFlight=10, partitioned QSFP platform
semantics and NIC R/platform integration. The next smallest NIC step is to
compose `routeHostMemoryReadResponses` on this production boundary and compare
SFC R ID selection, data/status/last propagation, invalid-ID blocking and
selected-sink backpressure while preserving CPU-stream identities.

Harness iteration 223 passed CIRCT replacertl/provenance, fresh Verilator
smoke, the portable 90,141-check suite and the 90,805-check Rocket/UART suite.
The Rocket differential still lacks its SFC UART-bearing baseline. Those
manager gates precede this change. This iteration starts no manager action;
new replacertl, Verilator and U250 verification remain harness-owned. The
overall port is incomplete, and unrelated shared edits remain outside this
checkpoint.


### Iteration 225 — compose production NIC host-memory R responses

The production compiler now invokes `routeHostMemoryReadResponses` for the
archived NIC target. The native pass creates FIRRTL operations for the
five-bit host R channel: ID 16 selects LoadMem, IDs 0–15 select FASED and trim
its ID to four bits, and IDs 17–31 stall. Payload broadcasts without valid
gating; host ready depends on the selected sink independently of host valid.
All five host AXI channels are joined at this boundary. NIC compilation stops
before output buffering with `NIC host memory output buffering and platform
integration are not implemented`; final NIC RTL and runtime remain pending.

The oracle inspection covered `passes/MidasTransforms.scala`,
`passes/fame/FAMETransform.scala`, `core/FPGATop.scala`'s AXI4Xbar construction,
and Rocket Chip `amba/axi4/Xbar.scala`'s `mapInputIds`, `trim` and `fanout`.
`PrintRocketHostMemoryReadResponsesCompare.py` exposes its binding comparison
for production boundaries and retains the strict historical Print counts.
Its SFC contract now checks all fifteen typed R ports and ten equations.
`NICHostMemoryReadResponsesCompare.py` checks the actual emitted wrapper,
unchanged prior modules, CPU streams, MMIO regions, raw annotation targets and
emitted annotation records, then executes the production helper's native SSA.

Native build and focused `goldengate-host-memory-read-responses` CTest pass:
8,192 exhaustive ID/status/last/valid/ready/payload cases, 65,536 leaf checks
and 3,840 unmatched-ID cases. Eleven required boundary bindings, eleven atomic
contract rejections, fourteen target identities and retained inner bodies
also pass. The same 8,192 cases and bindings pass on actual production NIC IR.
Two mutations change actual FIRRTL operations: LoadMem ID 16 becomes 17, and
FASED's ID slice becomes bits 4:1 instead of 3:0. Both pass native verification
but fail behavior (`AXI4Xbar R cone mismatch`) and structural comparison
(`shared R router differs`).

Local evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration225-u250-target-matrix/`.
`build.log`, `ctest.log`, `tool-input-hashes.json`, `nic-r-cli.json`, its behavior
log and the `nic-r-mutation/` artifacts record the tools and checks. Compiler
SHA256 is `ab8cea2fb2e63c11d6ab596b221029d45c0501b781ef4df022fc9a19f8de5402`.

Production composition passes with 223 unchanged prior module bodies, 117
copied ports, 118 wrapper ports and 126 connections. All 1,792 annotations,
26 CPU-stream ports and twelve MMIO regions remain; SimpleNIC remains slave 7
at byte address 512, size 32. The emitted R helper matches fresh no-NIC IR.
The NIC R boundary SHA256 is
`2d21e2d5b5b9cddf2f85ccb37c0a0c95aed45d90a93517e466bbec70ca09cb40`.
Fresh no-NIC composition also passes (209 prior bodies, 112 copied ports,
113 top ports, 121 connections), as do both historical Print orders
(279 prior bodies, 108 copied ports, 109 top ports, 117 connections and
21 retained Print modules).

The R contract matches `AXI4Xbar` in all three SFC artifacts:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.

`nic-r-comparison.json`, `no-nic-r-comparison.json`,
`immutable-r-comparison.json` and `golden-response-artifacts.json` record the
SFC comparisons. `fresh-no-nic-r-comparison.json` and `print-r-comparison.json`
record the other composition checks. `production-boundary-regression.json`
confirms all 244 earlier NIC and 256 no-NIC MLIR/JSON files match iteration 224
after output-directory normalization; NIC adds only two R boundary artifacts.
All five emitted no-NIC RTL/header/XDC files are byte-identical to iteration
224. The archived no-NIC FIRRTL and annotation hashes still match the immutable
staging handoff. Synthesis XDC matches the immutable U250 build-tree reference
exactly, and implementation XDC matches after the recorded
`target/FireSim_` to `target_FAMETop/FireSim` hierarchy substitution.

To make room for this replay, 578 old iteration-220 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz` files. Each decompressed SHA256 was checked
before removing the uncompressed copy; 36,507,560,103 bytes were recovered.
The iteration-224 comparison baseline, historical Print fixtures, active
compiler/build and immutable SFC fixtures were preserved.

The completed thirteen-recipe replay retains one compiled target, six compiler
gaps and six distinct unsupported partition/QSFP platforms. All archived input
hashes, effective platform configurations, compile statuses and earlier
boundary checks match iteration 224; only NIC's next-gap diagnostic advances.
All seven reached bridge/channel comparisons match. Saturn Shuttle's model
ports match; the other six reached model-port comparisons retain native-SRAM
differences requiring separate equivalence checks. Aggregate comparisons are
eight matched, six mismatched and twelve not reached. `replay/matrix.json`,
`matrix-regression.json` and the complete per-recipe stderr in
`matrix-regression.log` record these results. The replay exits one because
implementation gaps remain.

Other remaining gaps are multiple/non-Rocket TracerV constructors, FASED
outstanding profiles beyond maxFlight=10 and partitioned QSFP semantics.
The next smallest NIC step is to invoke `addHostMemoryOutputBuffer` on this
production boundary and compare its five two-entry AW/W/B/AR/R queues with
SFC `FPGATop.axi4buf`, preserving CPU-stream ports, MMIO and annotation targets.

Harness iteration 224 passed CIRCT replacertl/provenance and fresh Verilator
smoke; unchanged-input long-suite evidence was reused. The SFC UART-bearing
baseline remains pending. Those gates precede this change; this iteration
starts no manager action. New replacertl, Verilator and U250 verification
remain harness-owned. Local R routing evidence does not establish final NIC
platform/runtime parity or completion of the overall compiler port. Unrelated
shared-worktree edits remain outside this checkpoint.

### Iteration 226 — compose production NIC host-memory output queues

The production compiler now invokes `addHostMemoryOutputBuffer` for the
archived NIC target and emits `post-fame-host-memory-output-buffer.mlir` and
its complete annotation JSON. The native pass creates five independent
two-entry AW/W/B/AR/R queues through FIRRTL operations. AW and AR share a
module definition, with separate state in their instances. These queues have
neither flow nor pipe bypass. Reset clears occupancy, while an accepted
enqueue on the reset edge still writes payload RAM, as in the SFC oracle.
NIC stops before platform specialization with `NIC host memory platform
specialization and CPU stream integration are not implemented`.

Oracle inspection covered `passes/MidasTransforms.scala`,
`passes/fame/FAMETransform.scala`, and the AXI4Xbar-to-AXI4Buffer connection in
`core/FPGATop.scala`. The Scala sources are unchanged.
`PrintRocketHostMemoryOutputBufferCompare.py` now exposes its composition
comparison for production boundaries while retaining strict historical Print
counts. The SFC comparison discovers queue definitions through the five
AXI4Buffer instances: NIC uses Queue_24/25/26/28, whereas no-NIC uses
Queue_22/23/24/26. This observed naming difference drove the comparison change.
It also checks all 76 typed AXI4Buffer ports, 84 routing equations and queue
instance port bindings, including response direction and clock/reset.

`NICHostMemoryOutputBufferCompare.py` checks actual production composition,
annotations, CPU streams and MMIO, then executes the emitted queue SSA using
the native behavior test. Its mutation changes the address queue's actual
`not(full)` operation to `xor(full, full)`, preventing enqueue. Native CIRCT
verification accepts this type-correct mutation; behavior rejects it with
`no-flow/no-pipe handshake mismatch` and structural comparison rejects the
changed helper. A separate SFC mutation rewires AW reset to clock in a local
copy and is rejected by the routing comparison. Golden files are untouched.

Native compiler/test build and focused `goldengate-host-memory-output-buffer`
CTest pass. Tests cover 256 exhaustive queue states, 48 reset-edge RAM writes,
64 full states and 32 collisions, plus 6,000 five-channel hierarchical SSA
edges: 17,380 pushes, 17,147 pops, 8,350 stalls, 6,765 full observations,
9,487 simultaneous transfers and 44 resets. Ten atomic rejection cases and
eight boundary bindings also pass. The same exhaustive and hierarchical
behavior checks pass on the actual emitted production NIC boundary.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration226-u250-target-matrix/`.
`build.log`, `ctest.log`, `tool-input-hashes.json`, `nic-buffer-cli.json`, its
behavior log, and `nic-buffer-mutation/` record these checks. Compiler SHA256
is `046edf4621006778256b652447950d2028bcafb668539113ebec95c1cc0a29b6`.
The production NIC output-buffer boundary SHA256 is
`aeba43be7ba4611f063bce287b4dfe8a44a821ba7e423e29226a5eca1082dee7`.

Production composition passes with 224 unchanged prior module bodies,
118 public ports, 129 wrapper connections and five identical queue/helper
definitions. All 1,792 annotations, 26 CPU-stream ports and twelve MMIO regions
remain. SimpleNIC stays slave 7 at byte address 512, size 32; its token queue
depth 10 and CPU-stream queue depth 3,072 remain. Fresh no-NIC composition
passes with 210 prior bodies, 113 ports and 124 connections. Both historical
Print orders retain 280 prior bodies, 109 ports, 120 connections and 21 Print
modules. `nic-buffer-composition.json`, `fresh-no-nic-buffer-comparison.json`
and `print-buffer-comparison.json` record these results.

The five-channel queue and routing contract matches all three exact SFC
artifacts, recorded in `sfc-output-buffer-contracts.json`:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.

All four queue payload geometries (64/73/7/72 bits), depth-two handshake
equations, reset occupancy and reset-edge RAM write behavior match.
`production-boundary-regression.json` confirms 246 earlier NIC and 256 no-NIC
MLIR/JSON files match iteration 225 after output-directory normalization; NIC
adds only the two buffer artifacts. All five emitted no-NIC RTL/header/XDC
files are byte-identical to iteration 225. `immutable-rocket-comparison.json`
confirms archived no-NIC handoff hashes match the immutable staging fixture,
synthesis XDC matches the immutable U250 reference exactly, and implementation
XDC matches after the recorded hierarchy substitution.

To make room for the replay, 579 old iteration-221 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz` files. Decompressed SHA256 was checked before
removing each original; 36,553,439,648 bytes were recovered. Logs, JSON and RTL,
the iteration-225 baseline, historical Print fixtures, active compiler/build
and immutable fixtures were preserved. `storage.log` records this cleanup.

The completed thirteen-recipe replay retains one compiled target, six compiler
gaps and six distinct unsupported partition/QSFP platforms. All archived input
hashes, effective platforms, statuses and earlier boundary checks match
iteration 225; only NIC's next-gap diagnostic advances. Seven reached
bridge/channel comparisons match; Saturn Shuttle's model ports match, while
the other six reached model-port comparisons retain native-SRAM differences
requiring separate equivalence checks. Aggregate comparison counts are eight
matched, six mismatched and twelve not reached. `replay/matrix.json`,
`matrix-regression.json` and complete per-recipe stderr in
`matrix-regression.log` record these results. The replay exits one because
implementation gaps remain; the regression comparison passes.

Remaining gaps include multiple/non-Rocket TracerV constructors, FASED
outstanding profiles beyond maxFlight=10 and partitioned QSFP semantics.
The next smallest NIC step is to invoke `specializeHostMemoryPort` on this
buffered production boundary, compare the 5-bit-to-16-bit AXI ID adaptation
and `mem_0` bindings against SFC, and preserve CPU streams and annotations.
CPU-stream platform integration and final NIC RTL/runtime remain pending.

Harness iteration 225 passed CIRCT replacertl/provenance and fresh Verilator
smoke; unchanged-input long-suite evidence was reused. The SFC UART-bearing
baseline remains pending. Those gates precede this change. This iteration
starts no manager action; new replacertl, Verilator and U250 verification
remain harness-owned. Local output-buffer evidence does not establish final
NIC platform/runtime parity or completion of the compiler port. Unrelated
shared-worktree edits remain outside this checkpoint.

### Iteration 227 — compose production NIC host-memory platform IDs

`tools/goldengate.cpp` now invokes the native `specializeHostMemoryPort` pass
for the buffered NIC boundary. It emits and verifies
`post-fame-host-memory-platform.mlir` and its complete annotation archive
before stopping with `NIC CPU stream platform integration is not implemented`.
The pass constructs FIRRTL modules, instances, connections, `pad` and `bits`
operations: AW/AR IDs zero-extend from five to sixteen bits, B/R IDs truncate
to bits 4:0, and the remaining payload/handshake leaves pass through. There
is no validity gating or added state. Scala oracle sources remain unchanged.
Inspection covered `passes/MidasTransforms.scala`, `passes/fame/FAMETransform.scala`
and `core/FPGATop.scala`'s host-memory width check and `io <> bundle` connection.

`NICHostMemoryPortCompare.py` checks the actual production composition and
recursive annotation transfer, then invokes the C++ interpreter on its emitted
adapter SSA. `PrintRocketHostMemoryPortCompare.py` exposes the shared binding
comparison while retaining strict historical Print counts. The native test
interpreter now evaluates `firrtl.not`, so a type-correct request-ID inversion
is rejected for incorrect behavior, rather than an unsupported operation.
The second mutation changes response slicing from 4:0 to 5:1. Both mutants
pass CIRCT verification, fail actual SSA behavior with
`platform memory ID/handshake/payload mismatch`, and fail structural comparison.
A local SFC copy with the same incorrect B-response slice is also rejected.
No golden fixture is modified.

Native build and focused `goldengate-host-memory-port` CTest pass. The fixture
and actual production NIC adapter each pass 262,144 cases, covering all 65,536
response IDs, all 32 request IDs and four valid/ready combinations, totaling
9,699,328 leaf checks each. Twelve atomic rejection cases, ordinary/strict
clock/reset bindings, prior module bodies and copied/consumed target identity
checks also pass. Production composition retains 230 prior module bodies,
118 public ports, 117 copied ports and 119 wrapper connections. All 1,792
annotations, 26 CPU-stream ports and twelve MMIO regions remain; SimpleNIC
stays slave 7 at byte address 512, size 32. The depth-ten token queue and
depth-3,072 CPU-stream queue remain. Fresh no-NIC composition passes with
216 prior bodies, 113 ports and 114 connections. Both historical Print
composition orders pass with 286 prior bodies, 109 ports, 110 connections
and 21 Print modules.

All 37 typed `mem_0` ports and all 37 leaf equations match these exact SFC RTL
artifacts, including request extension, response truncation and handshake
independence:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration227-u250-target-matrix/`.
`build.log`, `behavior-build.log`, `ctest.log`, `tool-input-hashes.json`,
`sfc-platform-contracts.json`, `nic-platform-cli.json`, its behavior log,
`nic-platform-mutations/`, `nic-platform-composition.json`,
`fresh-no-nic-platform-comparison.json` and `print-platform-comparison.json`
record these checks. Compiler SHA256 is
`a64c591c6a4750e843d48c4cf2dce52e94830c424b715a565be8ca524c963511`;
production NIC platform boundary SHA256 is
`6b868dd1be8f69d7af7f08e85e2823760ae299a23e0ee81b3637d9de5eab95a7`.

`production-boundary-regression.json` confirms all 248 earlier NIC and 256
no-NIC MLIR/JSON files match iteration 226 after output-directory normalization;
only the two NIC platform artifacts are added. All five emitted no-NIC
RTL/header/XDC files are byte-identical to iteration 226.
`immutable-rocket-comparison.json` confirms the archived no-NIC FIRRTL and
annotation input hashes match the immutable staging fixture. Synthesis XDC
matches the immutable U250 reference exactly; implementation XDC matches
after the recorded hierarchy substitution.

The full thirteen-recipe replay retains one compiled target, six compiler
failures and six distinct unsupported partition/QSFP platforms. All archived
input hashes, effective platforms, statuses and earlier boundary comparison
checks match iteration 226; only NIC's next-gap diagnostic advances. Seven
reached bridge/channel boundaries match. Saturn Shuttle's model ports match;
the other six reached model-port boundaries retain native-SRAM differences
requiring separate equivalence checks. Aggregate counts are eight matched,
six mismatched and twelve not reached. `replay/matrix.json`,
`matrix-regression.json` and complete per-recipe stderr in
`matrix-regression.log` record these results. Replay exits one for the
remaining implementation gaps; the regression comparison exits zero.

To make replay space, 580 old iteration-222 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz` files. Decompressed SHA256 was verified
before removing each original, recovering 36,599,297,621 bytes. Logs, JSON,
RTL, iteration 226, historical Print fixtures, the active build and immutable
fixtures were preserved. `storage.log` records the operation.

The next smallest NIC step is to extend `CPUStreamPort.cpp` for the incoming
NIC CPU stream and its 512-bit W data, compare the aggregate slave interface
against SFC, and preserve existing buffered stream semantics. Final NIC
RTL/runtime, non-Rocket and multiple TracerV constructors, additional FASED
outstanding profiles, partitioned QSFP semantics and SRAM equivalence remain
pending. Harness iteration 226 passed CIRCT replacertl/provenance and fresh
Verilator smoke; unchanged-input long-suite evidence was reused, with the SFC
UART-bearing baseline still pending. Those gates precede this change. No
manager verification was started here; new replacertl, Verilator and U250
verification remain harness-owned. Local platform evidence does not establish
full NIC runtime parity or completion of the port. Unrelated shared-worktree
edits remain outside this checkpoint.

### Iteration 228 — compose the incoming NIC CPU AXI slave

`CPUStreamPort.cpp` now assembles the production NIC CPU stream platform
through FIRRTL modules, instances, subfields and connections. It accepts
one incoming stream with the recorded eighteen-bit writer address space,
adds the 512-bit W data route, and retains the nineteen-bit outgoing stream
contract. All 26 active scalar fields forward unconditionally. The 37-leaf
64/512/16-bit external AXI slave keeps eleven unused inputs: ten AW/AR
metadata fields and WLAST. No state or handshake conditions are added.
Inspection of `MidasTransforms.scala`, `FAMETransform.scala`,
`FPGATop.scala` and `CPUManagedStreamEngine.scala` confirms the platform
bulk connection, direct W payload assignment and AWLEN-based final-beat
counter. WLAST does not control the SFC writer.

`tools/goldengate.cpp` now emits and verifies the NIC
`post-fame-cpu-stream-platform.mlir` and complete annotation archive before
stopping at `NIC FPGA top shell integration is not implemented`.
`NICCPUStreamPortCompare.py` checks the actual emitted boundary, all copied
and consumed ports, exact aggregate connections, recursively transferred
annotations and unchanged prior bodies. The shared SFC CPU-port comparison
accepts the incoming profile while preserving historical Print/no-NIC checks.

The native build and focused `goldengate-cpu-stream-port` CTest pass.
Twenty atomic rejection cases preserve the input IR; positive zero/one-sink
profiles and catalog writer identity pass. Native checks of the emitted NIC
adapter prove all 26 routes are width-preserving identity connections for
every payload bit, independent of valid/ready. The two type-correct mutants
route W data from R data and AW address from AR address. Both pass native
CIRCT verification using `goldengate-simulation-master-control-test --verify-ir`,
then fail native CPU behavior and structured comparison. A locally mutated
SFC W-data assignment is also rejected; no golden was changed.

The production comparison retains 232 prior NIC modules, 92 copied ports,
93 public ports, 119 wrapper connections and all 1,792 annotations. Twelve
MMIO regions, the depth-ten NIC token queue and depth-3,072 CPU queue remain.
Fresh no-NIC composition retains 218 prior modules, 89 public ports and all
1,676 annotations. Both historical Print composition orders retain 297 prior
modules and pass their existing port, binding and annotation checks.

The exact compared SFC RTL artifacts are:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`:
  all 37 typed CPU AXI fields and 26 buffered identity routes match;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`:
  all 37 fields, 25 routes and twelve unused inputs match;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`:
  the same 37-field, 25-route zero-sink contract matches.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration228-u250-target-matrix/`.
`build-retry.log`, `ctest.log`, `nic-cpu-platform.json`, its behavior log,
`nic-cpu-platform-mutations-verified/`, `no-nic-cpu-platform.json`,
`sfc-cpu-contracts.json`, `sfc-contract-mutation.json` and
`print-platform-comparison.json` record these checks. Compiler SHA256 is
`cb03dba10bd7b3b572e5de98a569bc99c38f56012fdf16da40dfda69b3fd1d8e`;
the fresh production NIC CPU platform boundary SHA256 is
`86bd161ba3f1a40c18949360e8dde5da78a3ac865e052534a8e24bb0b38de520`.

`production-boundary-regression.json` confirms all 250 earlier NIC and 256
no-NIC MLIR/JSON files match iteration 227 after output-directory normalization;
only the two new NIC CPU platform artifacts are added. All five emitted
no-NIC RTL/header/XDC files are byte-identical to iteration 227.
`immutable-rocket-comparison.json` confirms both archived no-NIC input hashes
match the immutable staging fixture, synthesis XDC matches the immutable
U250 reference exactly, and implementation XDC matches after the recorded
hierarchy substitution.

The full thirteen-recipe replay retains one compiled target, six compiler
failures and six distinct unsupported partition/QSFP platforms. All archived
input hashes, effective platforms, statuses and earlier boundary checks match
iteration 227; NIC alone advances to the FPGA shell diagnostic. Seven reached
bridge/channel boundaries match. Saturn Shuttle model ports match; the other
six reached model-port boundaries retain native-SRAM differences requiring
separate equivalence checks. Aggregate comparisons remain eight matched, six
mismatched and twelve not reached. `replay/matrix.json`,
`matrix-regression.json` and `matrix-regression.log` record every row and
complete per-recipe stderr. Replay exits one for those implementation gaps;
the regression comparison exits zero.

To make replay space, 581 old iteration-223 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz` files. Decompressed SHA256 was verified
before removing each original, recovering 36,645,153,753 bytes. Logs, JSON,
RTL, iteration 227, historical Print fixtures, the active build and immutable
fixtures were preserved. `storage.log` records the operation.

The next smallest NIC step is to compose and compare the FPGA top shell,
including its host clock/reset and five public platform interfaces, before
advancing to F1 shim/header emission. NIC final RTL/runtime, non-Rocket and
multiple TracerV constructors, other FASED outstanding profiles, partitioned
QSFP semantics and SRAM equivalence remain pending. Harness iteration 227
passed CIRCT replacertl/provenance and fresh Verilator smoke; unchanged-input
long-suite evidence was reused, with the SFC UART-bearing baseline pending.
Those gates precede this change. No manager verification was started here;
new replacertl, Verilator and U250 verification remain harness-owned. Local
boundary checks do not establish full NIC runtime parity or port completion.
Unrelated shared-worktree edits remain outside this checkpoint.

### Iteration 229 — compose the incoming NIC FPGA top shell

`tools/goldengate.cpp` now runs the existing CIRCT `assembleFPGATopShell`
implementation for the incoming NIC profile, verifies the resulting module,
and emits `post-fame-fpga-top.mlir` and its complete annotation archive.
The explicit unsupported boundary advances from the FPGA shell to
`NIC F1 shim and header integration is not implemented`. This is actual
compiler composition through FIRRTL operations; it does not emit final NIC
simulator RTL yet. Inspection of `MidasTransforms.scala`, `FAMETransform.scala`
and `core/FPGATop.scala` confirms the stage order, direct host clock/reset
connections, three public AXI interfaces and `HostClockSource` identity.

`FPGATopShellTest.cpp` now inspects an actual production boundary with
`--platform-boundary`, without reapplying the pass or fabricating annotations.
It requires one retained child and exactly five unconditional aggregate
connections, no added operations, exact public types/directions/identities,
and unique FAME host clock/reset and host clock source. The focused native
build and `goldengate-fpga-top-shell` CTest pass, including the existing
expanded Print profile and 21 atomic rejection cases.

`NICFPGATopShellCompare.py` compares the actual emitted boundary and full
annotation transfer, then lowers only the new shell with an external child
declaration for the ABI comparison. This test fixture is not a replacement
NIC simulator candidate. All 234 prior NIC module bodies remain unchanged;
five public aggregate ports, 88 retained private ports, five identity
connections and all 1,793 annotations match. All 121 lowered public leaves
match SFC names, directions, widths and unconditional bindings. SFC has twenty
direct host-clock and twenty direct host-reset consumers at this boundary.

Two actual-MLIR mutants disconnect the host clock or route reset from an
existing diagnostic signal. Both pass CIRCT verification, then fail the
native identity check and structured comparison. A local copy of SFC with
the CPU stream engine reset driven by clock also fails the independent
oracle fanout check. No immutable golden was modified. These checks prove
the shell's unconditional identities for arbitrary payload/handshake values;
they do not establish behavior of its retained child or full NIC runtime.

The exact SFC artifacts compared are:

- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv`,
  SHA256 `bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`:
  all 121 shell leaves and five aggregate identities match;
- `automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_no_nic/design/FireSim-generated.sv`,
  SHA256 `2c4960ccd0f4bfabff888cbd7d48867eac21f19ac99a4ee105f82c30464ff702`:
  the same 121-leaf shell contract matches, retaining 220 prior modules,
  84 private ports and all 1,677 annotations;
- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
  SHA256 `20c6ae241b9fbd0cdcab98da3387eb754d45a96c6bc347811982785fa0db1243`:
  all 121 shell leaves and five aggregate identities match.

Both historical Print shell composition orders retain 299 prior modules,
21 Print modules, 93 private ports and their complete annotation archives;
the same immutable SFC ABI matches. Historical fixtures seed their host
identities and the SFC oracle disables printf. This is regression coverage,
not enabled-Print runtime equivalence.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration229-u250-target-matrix/`.
`build.log`, `ctest.log`, `nic-shell/comparison.json`, `no-nic-shell/comparison.json`,
`immutable-shell/comparison.json` and `historical-print-shell.json` record the
focused checks, including lowering, native identity and mutation logs.
Compiler SHA256 is
`b8a7754bfd16b40af071b354c7c3d95d4a83f2a2bb43ad2f103eafe2acd1128d`;
the new production NIC shell boundary SHA256 is
`6b471e981a106955a3535117158e0c4392fd4cbdbefd98cc2bc1d8da22d40b6e`.

`production-boundary-regression.json` confirms all 252 earlier NIC and 256
no-NIC MLIR/JSON files match iteration 228 after output-directory normalization;
only the two NIC FPGA shell artifacts are new. All five no-NIC emitted
RTL/header/XDC files are byte-identical to iteration 228.
`immutable-rocket-comparison.json` confirms archived no-NIC FIRRTL/annotation
hashes match the immutable staging fixture, synthesis XDC matches the
immutable U250 reference exactly, and implementation XDC matches after the
recorded hierarchy substitution.

Fresh replay of all thirteen recipes retains one compiled target, six
compiler failures and six distinct unsupported partition/QSFP platforms.
Archived input hashes, effective platforms, statuses and earlier boundary
checks match iteration 228; NIC alone advances to the F1 shim/header
diagnostic. Seven bridge/channel boundaries and Saturn Shuttle model ports
match. Six reached model-port boundaries retain native-SRAM differences
requiring separate equivalence checks. Aggregate comparisons remain eight
matched, six mismatched and twelve not reached. `replay/matrix.json`,
`matrix-regression.json` and `matrix-regression.log` record every result and
complete per-recipe stderr. Replay exits one for implementation gaps;
the regression comparison exits zero.

To make replay space, 584 old iteration-224 uncompressed MLIR files were
losslessly compressed to adjacent `.mlir.gz` files, recovering 36,782,678,718
bytes. Decompressed SHA256 was verified before removing each original.
`storage.log` records the operation. Logs, JSON, RTL, iteration 228,
historical Print fixtures, the active build and immutable fixtures remain.

Harness iteration 228 passed CIRCT replacertl/provenance, fresh Verilator
smoke, both fresh long suites and U250 buildbitstream. The portable suite
passed 90,141 checks against Spike; Rocket passed 90,805 checks with its SFC
UART-bearing baseline still pending. Those gates precede this change.
The supplied complete hardware infrasetup stdout/stderr and manager log
`deploy/logs/2026-10-15--10-57-48-infrasetup-S1PC3ZVAOFCR9VIV.log` show the
FPGA database returning `[]`, followed by `Less FPGAs available than slots
(0 >= 0)` before programming. No compiler change is indicated by that host
inventory failure. No manager verification was started here.

The next smallest NIC step is to compose and compare its F1 shim and driver
header interfaces against SFC before final RTL/runtime validation. Additional
TracerV constructors, other FASED outstanding profiles, partitioned QSFP
semantics and SRAM equivalence remain pending. New verification remains
harness-owned; this boundary milestone does not establish port completion.
Unrelated shared-worktree edits remain outside this checkpoint.

### Iteration 230 — incoming NIC U250 F1 shim

The production CLI now invokes `assembleF1Shim` for the incoming-NIC target
after its verified FPGA shell. This reuses the native FIRRTL-operation
implementation of Scala `F1Shim.scala`: PCIS forwarding, independent host
AW/AR request-ID counters, inactive physical QSFP lanes, memory forwarding,
DDR defines and XDC circuit paths. `MidasTransforms.scala` places this
platform specialization after FAME and SimulationMapping; target scheduling
and all existing model/bridge bodies remain intact. NIC now stops explicitly
at `NIC metasim and driver header integration is not implemented`, after
emitting the actual shim IR/archive. The no-NIC header ordering is unchanged.

`F1ShimTest.cpp --platform-boundary` parses and verifies this production IR,
checks its two synchronous 12-bit counters, unique platform child and retained
host-clock source, and emits the shim with an external FPGATop declaration
for focused lowering. Its collateral checks reject five invalid output sets
before writes. Normal `goldengate-f1-shim` CTest passes, including the twelve
existing atomic transform rejections. Native compiler/test rebuild passes.

`Transforms/NICF1ShimCompare.py` compares the production FPGA-top/shim
boundaries, verifies all 235 earlier module bodies are unchanged, and checks
the complete recursive annotation transfer (1,793 records become 1,795 with
DDR/XDC records). There is exactly one retained source,
`~F1Shim|FPGATop>clock`; the new shim body also equals the production no-NIC
shim. Lowered ports and every unconditional route match both these SFC files:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The NIC RTL SHA256 is
`bbffd83a60547249537029e4cb117c578fe830f006d1627f37d53dba3cf824af`.
Both comparisons match all 143 public port leaves and 129 combinational
routes. The actual native counter-operation DAG matches SFC's synchronous
reset/accepted-request recurrence over 30,000 host cycles: three reset
cycles, 31,508 independent counter stalls and two wraps per counter. Four
counter mutations (reset value, increment, truncation and accepted-request
enable) are rejected. Both companion `FireSim-generated.defines.vh` files
match emitted DDR collateral byte for byte. Native annotation re-emission
also matches the CLI archive exactly. These focused checks externalize the
retained FPGATop only for testing; they do not establish complete NIC RTL or
runtime equivalence.

Evidence is under the active generated directory's
`iteration230-u250-target-matrix/`: `nic-shim/comparison.json`,
`immutable-shim/comparison.json`, their native/lowering logs, `build.log`,
`ctest.log` and `inputs.json`. Compiler SHA256 is
`004daf313d5c22e1577780476cebb255b8053687a49af6e597b7bd49960faded`.

Fresh replay of all thirteen archived pre-MIDAS inputs retains one compiled
target, six compiler failures and six unsupported partition/QSFP platforms.
Earlier input hashes, effective platforms, statuses and boundary comparisons
match iteration 229; NIC alone advances to the header diagnostic. Seven
bridge/channel boundaries and Saturn Shuttle model ports match; six reached
model-port comparisons retain native-SRAM differences. Aggregate annotation
comparisons remain eight matched, six mismatched and twelve not reached.
`replay/matrix.json`, `matrix-regression.json` and `matrix-regression.log`
record each recipe and its complete stderr. Replay exits one for these
implementation gaps; regression checks exit zero.

`production-boundary-regression.json` confirms all 254 earlier NIC and 256
no-NIC MLIR/JSON files match iteration 229 after output-directory normalization;
only the NIC shim IR/archive are new. All five no-NIC emitted RTL/header/XDC
artifacts are byte-identical to iteration 229. The immutable Rocket comparison
again checks archived input hashes against the staging fixture, exact
synthesis XDC, and implementation XDC after the recorded hierarchy rename.

To make comparison space, 585 old iteration-225 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz` files, recovering 36,828,624,680 bytes.
Decompressed SHA256 was checked before each original was removed. The
adjacent `iteration230-u250-target-matrix-storage.log` records every file.
JSON, logs, final RTL, prior baseline 229, the active build, historical Print
fixtures and all immutable references remain intact.

Supplied harness iteration 229 passed CIRCT replacertl/provenance, fresh
Verilator smoke and both fresh long Rocket suites. Portable checks total
90,141 against Spike; Rocket checks total 90,805 with the SFC UART-bearing
baseline still pending. Those results precede this change; no manager
verification was started here.

The next smallest NIC step is to emit and compare its metasim interface
header before enabling bridge driver constructors and final RTL/runtime
validation. Other TracerV constructors, FASED profiles, partitioned QSFP and
SRAM equivalence remain pending. This shim milestone does not complete the
port. Unrelated shared-worktree edits remain outside this checkpoint.

### Iteration 231 — incoming NIC metasim interface headers

The production CLI now applies `prepareMetasimInterfaceHeader` to the actual
NIC F1Shim after its existing retained boundary. This ports the physical
interface-header step of Scala `SimulationMapping.generateHeaderAnnos` and
`FPGATopImp.genHeader/genVHeader` to the NIC pipeline, deriving constants from
native FIRRTL port types. The compiler emits
`post-fame-metasim-header-all.json` and the three retained OutputFile bodies
under `metasim-interface/`. The partial C++ header contains the typed metasim
configuration; NIC bridge constructors remain pending. Compilation therefore
still rejects explicitly at `NIC driver header integration is not implemented`.
The no-NIC driver/header path is unchanged.

`Support/NICMetasimInterfaceCompare.py` compares these production outputs
against the completed NIC SFC artifacts:

```text
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.h
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.const.vh
/scratch/jfx/fsim-circt/automation/chia_firesim_circt/golden/u250-sfc-20261013/alveo_u250_firesim_rocket_singlecore_nic/design/FireSim-generated.sv
```

The complete typed configuration matches after whitespace and equivalent C
string normalization. All twelve usable Verilog constants match; CIRCT omits
Scala's unused malformed `MEM_HAS_CHANNEL-1` macro. The original FireSim
identity, optional CPU/FPGA endpoints, memory count and zero target QSFP
channels are checked. Both candidate and SFC C++ configurations compile
against the actual `midas/src/main/cc/core/config.h` types with static
assertions. This is a configuration ABI check, not execution of NIC drivers.

The comparison also checks 31 width/direction relationships against each
of the NIC RTL above and the immutable U250 RTL:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

All 62 checks match. This immutable build-tree fixture has no `.const.h` or
`.const.vh`; its comparison uses the actual RTL ports, without inventing a
missing header. The initial test assumed a companion C++ header there and
failed with FileNotFoundError; the corrected comparison records that absence.
Native `MetasimInterfaceHeaderTest` verifies the complete production MLIR
before and after preparation, preserves every module body and all 1,795
existing annotation records, and independently emits the same three files
and complete 1,797-record annotation archive as the CLI. Normal header and
F1-shim CTests pass, including varied-width and atomic rejection cases.

Evidence is under the active generated directory's
`iteration231-u250-target-matrix/`: `nic-metasim/comparison.json`, native and
C++ ABI logs, `build.log`, `ctest.log`, `inputs.json`, and the complete
`replay/` directory. Compiler SHA256 is
`0857ecda77e0c84b00a06f2b7cd53b466bea30007c2fd2f457571b5e99d7e549`.

The thirteen-target replay retains one compiled target, six compiler failures
and six unsupported partition/QSFP platforms. Archived input hashes,
effective platforms, earlier comparison results and statuses match iteration
230; NIC alone advances to its driver-header diagnostic. Seven bridge/channel
boundaries and Shuttle model ports match; six model-port boundaries retain
the native-SRAM differences. Aggregate comparisons remain eight matched,
six mismatched and twelve not reached. These gaps remain failed comparisons.
`matrix-regression.json` and its log record every recipe and complete stderr.

All 256 earlier NIC and 256 no-NIC MLIR/JSON files match iteration 230 after
output-directory normalization; only the NIC header archive is new. All five
final no-NIC RTL/header/XDC artifacts are byte-identical. The archived Rocket
inputs again match the immutable staging fixture by SHA256; synthesis XDC
matches the immutable U250 fixture exactly and implementation XDC matches
after the documented hierarchy rename. Artifact reports retain exact paths.

To make replay space, 584 old iteration-226 MLIR dumps were losslessly
compressed to adjacent `.mlir.gz`, recovering 36,783,084,286 bytes after
checking decompressed SHA256. `storage.log` records every file. JSON, RTL,
logs, active/prior baselines, historical Print and immutable fixtures remain.

Supplied harness iteration 230 passed CIRCT replacertl/provenance and fresh
Verilator smoke; unchanged-input long-suite evidence was reused from earlier
passing runs, with the SFC UART-bearing baseline still pending. These gates
precede this change. No manager verification was started in this iteration.

The next smallest NIC step is to extend `prepareCPUManagedStreamHeader` for
the validated incoming queue allocation/count/data paths, then add the
SimpleNIC constructor with allocated MMIO and ordered stream indices from
the Scala oracle. Complete NIC RTL/runtime validation, other target gaps,
partitioned QSFP and SRAM equivalence remain pending. This header boundary
does not complete the port. Unrelated shared-worktree edits are excluded.
