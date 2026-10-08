# Native SRAM extraction boundary

`labelSRAMModels` ports the optional `LabelSRAMModels.scala` transform using
native FIRRTL memory, module, instance, selector, and connect operations.
It selects local memory identities with a set, creates one wrapper per memory,
allocates names in the circuit namespace, and preserves the cloned memory's
depth, data/mask types, latencies, RUW, and attributes. Wrapper ports are one
`clk`, then read, write, and readwrite bundles with their clock fields removed.
Flipped read data flows out of the wrapper. Parent clock connects redirect to
the instance's single clock in their original order, including last-connect
behavior. Consumed memory labels become FAME instance, NoDedup, and SRAM port
annotations; other retained records remain intact.

The compiler exposes `--label-sram-models` and emits
`post-wrap-sram-models.mlir` and `post-wrap-sram-models-all.json`. Its native
CHIRRTL/width/reset normalization deliberately precedes extraction without
LowerTypes, matching the ordering in `MidasTransforms.scala` and preserving
aggregate memory identity. The recorded default Rocket pipeline does not enable
`GenerateMultiCycleRamModels`; this explicit boundary does not change that default.

The CTest `goldengate-label-sram-models` verifies clone attributes, namespace
collisions, port directions/order, output and clock connections, duplicate
selection, retained metadata, aggregate data/masks, and idempotence. Memory
inner symbols and whole-port aggregate users require further identity/expansion
work and are rejected before mutation; tests check both rejection paths.

`LabelSRAMModelsOracle.scala` runs the unchanged SFC HighFirrtlCompiler and
LabelSRAMModels on mixed-port, namespace/multiple-clock, and Rocket register-file
probes. An optional second argument runs the same oracle on the immutable full
Rocket FIRRTL. Iteration 41 evidence is in the existing generated directory's
`iteration41-sram-extraction/`: `ctest.log`, `oracle.log`, four candidate
directories, `comparison.log`, and `golden-sram-boundary-report.json`.

Four fresh native/SFC boundaries match all 20 new annotation records, 46 wrapper
data/control field shapes and connection directions, and 12 internal clock
connections. Both compilers extract exactly one wrapper for duplicate memory
labels. Fresh native compilation of the full archived input verifies and emits
the optional boundary with empty stderr. The exact immutable artifacts compared
are:

```text
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`~FireSim|Rocket>rf` selects the same depth-31, UInt64, two-read/one-write
register file, with read latency zero and write latency one. All ten archived
RTL data/address/enable/mask names and widths match the cloned memory's wrapper
fields. The new instance target `~FireSim|Rocket/rf:rf`, wrapper `rf`, four
ordered wrapper ports, and five annotation records match fresh SFC optional
extraction on that exact archived FIRRTL. The original recorded RTL keeps `rf`
inline: whole transformed RTL and retained annotation arrays are not declared
equivalent. Only the selected memory's behavior and optional extraction boundary
were compared.

Iteration 44 adds transitive SRAM promotion and checks retained-target
LowerTypes identities; see [ExtractSRAMModels.md](ExtractSRAMModels.md) for
the fresh Scala comparisons. Remaining work includes SRAM FAME control,
abstract RAM timing/host models, native handling of symbolic memory targets,
and general port-order behavior beyond these probes. The next small step is
FAMEDefaults and InferModelPorts on the promoted SRAMs. The SFC UART-bearing
baseline and overall migration remain incomplete.
