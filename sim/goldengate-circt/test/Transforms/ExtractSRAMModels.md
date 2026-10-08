# SRAM promotion through ExtractModel

The native `ExtractModel` follows the unchanged Scala transform's recursive
promotion to the circuit main. Each removed model instance becomes an input
bundle on its parent. CIRCT creates a peer model beside every parent use,
expands nonpassive connections with `emitConnect`, and carries the model label
to each new peer until all branches reach the top. Completed and duplicate
model labels are consumed; SRAM module-port annotations remain unchanged.
The promotion count measures removed instances, so fanout can produce more
top-level models than promotions.

`--extract-sram-models` runs the native CHIRRTL/width/reset normalization,
`LabelSRAMModels`, and `ExtractModel`. It emits both the wrapper boundary and
`post-extract-model.fir` / `post-extract-model-all.json`. This explicit boundary
does not enable the optional RAM pipeline in the default Rocket compilation.
Feeding its outputs to `--lower-types` exercises retained aggregate target
renaming, including read-data flips and address/data/enable/mask identities.

CTest `goldengate-extract-model` checks two levels of repeated parent uses,
four top-level peers, read-data and clock directions, preserved model bodies,
duplicate labels, already-top cleanup, and idempotence. Nested analog leaves
and unported external annotations referring to the removed instance are
rejected before that promotion mutates the circuit. The existing
`goldengate-label-sram-models` test also passes.

`ExtractSRAMModelsOracle.scala` runs the unchanged SFC LowFirrtlCompiler,
RemoveValidIf, LabelSRAMModels, ExtractModel, and subsequent LowForm lowering.
LowForm and RemoveValidIf preparation reflect the optional transform's
ordering in `MidasTransforms.scala`; both candidates receive the same typed
handoff. The oracle exercises a two-level fanout memory with read, write and
readwrite ports and the register file selected by `~FireSim|Rocket>rf` in this
exact immutable artifact:

```text
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

Iteration 44 evidence resides under the current U250 generated directory in
`iteration44-sram-promotion/`. `oracle.log`, `ctest.log`, `comparison.log`, and
`golden-comparison.json` record the results. Structured comparisons pass:

| Boundary | Top-level SRAMs | Model connections | Internal clock connections | Lowered model port leaves | Annotation records |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fanout | 4 | 99 | 3 | 56 | 4 |
| Immutable Rocket register file | 1 | 77 | 3 | 77 | 4 |

Rocket takes six promotions. Both comparisons include aggregate port shapes,
connection directions and multiplicities, memory depth/types/latencies/RUW,
all pre-lowering annotation records, and all lowered SRAM targets and port
directions/widths. The post-lowering annotation comparison excludes only SFC's
`firrtl.transforms.DedupedResult` bookkeeping: the native LowerTypes boundary
does not run circuit deduplication. An additional native run ingests the
immutable FIRRTL directly and verifies six-level promotion.

A fresh default native compilation through FAME and PrintBridge host binding
passes with empty stderr (`baseline.stdout` / `baseline.stderr`). Iteration 43
harness-owned CIRCT replacertl and required Verilator regressions passed before
this change; iteration 44 manager gates remain harness-owned. This comparison
does not establish equivalence between optional SRAM extraction and the
archived default whole RTL.

Next, feed the promoted SRAMs through FAMEDefaults and InferModelPorts and compare
their clock-domain and channel identities with SFC before extending FAME
control and abstract RAM timing models. Symbolic memory targets, general
annotation renaming across promotion, and the SFC UART-bearing baseline remain
unfinished.
