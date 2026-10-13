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
