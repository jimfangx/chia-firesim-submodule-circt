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
