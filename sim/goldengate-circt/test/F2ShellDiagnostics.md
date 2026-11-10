# F2 FRFCFS shell diagnostics (iteration 11)

CHIA iteration 10 emits a complete Rocket candidate. The three selected DDR3
targets reach FPGA shell assembly, then reject `unrecognized diagnostic output`.
The post-MMIO native FRFCFS component pipeline retains four observation vectors:
`fased_frfcfs_dram_entries`, `fased_frfcfs_dram_rankStati`,
`fased_frfcfs_llc_mshrEntries` and `fased_frfcfs_llc_metadata`. Rank status contains
a second vector of bank status. Every leaf is an output of known-width UInt type.
The shell's validator previously supported only bundles and UInt leaves.

`FPGATopShell.cpp` now traverses diagnostic vectors while preserving polarity
through every bundle flip. Name-prefix and output-direction guards remain;
clocks, resets, unknown-width leaves and hidden inputs reject before mutation.
Public AXI validation still rejects vectors, including empty extra fields. The
shell still adds only its existing five aggregate identity connections and one
instance. Inner module bodies and diagnostic annotation targets remain intact.
FRFCFS histograms and rank-power vectors have already been connected to MMIO by
`attachFASEDFRFCFSMMIO`; they are not among the newly internal observations.

The Scala oracle is `midas/core/FPGATop.scala` (`FPGATopImp` public interfaces)
and `midas/models/dram/FASEDMemoryTimingModel.scala` (model MMIO attachment).
Neither the oracle nor any immutable golden is changed.

From the repository root, focused optimized checks are:

```sh
.conda-env/bin/cmake -S sims/firesim/sim/goldengate-circt \
  -B /scratch/jfx/goldengate-circt-f2-release-build -DBUILD_TESTING=ON
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-release-build \
  --parallel 8 --target goldengate-circt goldengate-fpga-top-shell-test \
  goldengate-f1-shim-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-release-build \
  --output-on-failure \
  -R '^goldengate-(fpga-top-shell|f1-shim|f2-compiler-config|debug-snapshot-policy)$'
```

All four CTests pass under Release `-O3 -DNDEBUG`. Shell tests check U250 and F2
passive vectors, nested input flips, clock/reset/unknown-width leaves, strict
public contracts, exact forwarding, retained bodies/annotations and 33 atomic
rejections. A reconstructed pre-fix implementation linked to the new component
test reproduces rejection on the first retained FRFCFS vector.

Existing component pipeline fixtures supply actual post-MMIO diagnostic types:

```sh
/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-frfcfs-pipeline-test \
  --fixture 10 /scratch/jfx/goldengate-circt-f2-build/iteration11-frfcfs10.mlir
/scratch/jfx/goldengate-circt-f2-release-build/goldengate-fpga-top-shell-test \
  --frfcfs-diagnostics /scratch/jfx/goldengate-circt-f2-build/iteration11-frfcfs10.mlir
```

Repeat those commands with `20` and `iteration11-frfcfs20.mlir`. Both profiles
pass: four production types are inserted individually into F2 boundary scaffolds
and verified. This checks type compatibility and unchanged shell connections;
the scaffold is not a complete selected target or a behavioral comparison.

A separate static audit compares the native F2 public contract with all 121
scalar ports of each exact immutable SFC `FPGATop`: Rocket, MegaBoomV3, plain
Shuttle and Saturn Shuttle. Names, widths and directions match after flattening
aggregate names with underscores, interpreting implicit scalar widths as one,
and ignoring declaration order/comments. Complete archived prerequisites
revalidate; receipt/manifest/full RTL hashes equal iteration 10. This audit
does not compare emitted candidate interfaces or establish semantic parity.

Evidence is under `/scratch/jfx/goldengate-circt-f2-build/`:
`iteration11-release-build-final.log`, `iteration11-release-ctest.log`,
`iteration11-before-reproduction.json`, `iteration11-frfcfs-shell-result.json`,
`iteration11-shell-golden-audit.py`, `iteration11-shell-golden-audit.json`,
`iteration11-golden-identities.json` and `iteration11-implementation-result.json`.
CHIA must retry the four complete archived-input compilations, then run full
syntax/normalized semantic comparisons. Separate Jasper LEC/SEC, F2 runtime
and matched faster compiler timing remain outstanding. Acceptance adapters
remain intentionally unset. No duplicate long verification job was launched.
