# Baseline SRAM configuration

The supported baseline platform profiles inherit
`GenerateMultiCycleRamModels=false`. A retained
`midas.targetutils.FirrtlMemModelAnnotation` selects a memory for the optional
pass; it does not enable that pass. `tools/goldengate.cpp` now preserves this
default before `ExtractModel`. Unsupported platform fragments still reject.
The explicit `--label-sram-models` and `--extract-sram-models` boundaries retain
their existing behavior.

The unchanged Scala contract is in `midas/Config.scala:67` and
`midas/passes/MidasTransforms.scala:20–26`. `BaseF2Config`, the selected
`FRFCFS16GBQuadRankLLC4MB` fragment and `WithAutoILA` never enable the parameter.
All four immutable F2 SFC transformations omit optional SRAM extraction.

The archived iteration-11 native candidates incorrectly extract one Rocket
register file and two MegaBoom register files. Complete selected annotation
records show 10 extra Rocket channels/20 extra model-port annotations, and 94
extra MegaBoom channels/188 extra model-port annotations. Shuttle and Saturn
have no selected memories. These differences are compiler errors, not accepted
normalizations. The new compiler has not been replayed on those full targets in
this implementation call; CHIA owns that replay.

`goldengate-baseline-sram-config` runs the real importer and baseline pipeline
on a depth-31 UInt64 memory with asynchronous reads, synchronous writes, a
one-bit write mask, read-to-write feedback and a memory selection annotation.
It checks inline geometry and selection retention without added SRAM model
ports for F2 LBP, the exact F2 FRFCFS/AutoILA profile and U250. The clock-only
fixture deliberately stops at the later no-data-channel FAME diagnostic. Both
explicit extraction commands must still succeed and emit their read/write
port annotations. This is a compiler configuration regression, not a target
runtime or memory-behavior equivalence test.

Iteration 12 commands, from the repository root:

```sh
.conda-env/bin/cmake -S sims/firesim/sim/goldengate-circt \
  -B /scratch/jfx/goldengate-circt-f2-release-build -DBUILD_TESTING=ON
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-release-build \
  --parallel 8 --target goldengate-circt goldengate-label-sram-models-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-release-build \
  --output-on-failure \
  -R '^goldengate-(baseline-sram-config|label-sram-models|f2-compiler-config|debug-snapshot-policy)$'
```

The Release compiler builds and all four focused CTests pass. Before the fix,
the new fixture promotes the memory and rejects the added model connection;
logs remain under
`/scratch/jfx/goldengate-circt-f2-build/iteration12-sram-regression-before/`.

Read-only comparisons use the actual four immutable SFC outputs and archived
iteration-11 candidates. Each matches 121 FPGATop ports, 174 F1Shim ports, 155
shim routes, eight bridge-extraction constructors and its 43/71/51/51 channels.
The four headers each match eleven driver constructors and 88 fixed-layout
assertions. Defines and synthesis XDC match; implementation XDC matches after
stanza ordering and an explicitly resolved hierarchy substitution. These are
scoped static checks, with no RTL/driver syntax or internal behavioral claim.
The partial CHIA adapter documents remaining obligations separately in
[Support/CompareF2Semantics.md](Support/CompareF2Semantics.md).

Evidence under `/scratch/jfx/goldengate-circt-f2-build/`:

- `iteration12-release-ctest.log` and `iteration12-release-ctest-command.json`
- `iteration12-annotation-boundary-audit.json`
- `iteration12-interface-audit.py` and `iteration12-interface-audit.json`
- `iteration12-collateral-audit.py` and `iteration12-collateral-audit.json`
- `iteration12-golden-validation.json` and `iteration12-golden-identities.json`

The four immutable receipt/manifest/full-RTL hashes equal iteration 11; full
archived input/resource/output hashes revalidate. Scala oracle sources are
unchanged. Full paired syntax/semantic comparisons, independent Jasper LEC and
SEC, F2 bitstream/runtime and faster matched compiler timing remain incomplete.
