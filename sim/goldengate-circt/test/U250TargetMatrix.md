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
