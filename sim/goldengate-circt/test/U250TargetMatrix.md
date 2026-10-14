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
