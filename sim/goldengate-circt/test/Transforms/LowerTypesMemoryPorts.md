# SRAM model port identities through target normalization

SFC `LabelSRAMModels` builds aggregate wrapper ports for an annotated memory.
`ModelReadPort`, `ModelWritePort`, and `ModelReadWritePort` identify their data,
mask, address, enable and mode fields. Each annotation's `update` uses
`RTRenamer.exact`; a member must rename to one surviving ground reference.
`MidasTransforms` enables extraction with `GenerateMultiCycleRamModels`, then
lowers the target before FAME. `EmitAndWrapRAMModels` consumes these annotations
later, after FAME and model threading.

Native `LowerTypes` previously preserved those records verbatim, leaving
selectors such as `~Top|ram>r.data` after the wrapper port became `r_data`.
It now transfers all 13 required members through CIRCT field IDs and inner
symbols. Singleton bundles/vectors, including aggregates whose other leaves
have zero width, select their sole surviving leaf. Multiple-leaf aggregates
and zero-width selections reject; missing, non-reference
and unresolved members reject before any IR mutation. Vector element selectors
and port/node/wire declarations use the same native identity transfer, including
namespace collisions. Late debug-only lowering preserves historical SRAM
metadata without validating or rewriting its consumed targets.

## Executable oracle and local checks

`LowerTypesMemoryPortsOracle.scala` invokes production SFC HighFirrtlCompiler,
`LabelSRAMModels`, LowFirrtlCompiler, and the three actual Golden Gate classes.
It emits scalar, singleton-bundle, singleton-vector, padded-singleton,
zero-width and aggregate-data wrapper fixtures and annotations.
Run it with the compiled MIDAS dependency classpath, then run the native tool
on the emitted `scalar.fir` and `scalar.json` with `--lower-types`.

Iteration 40 evidence lives in
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration40-memory-target-lowering/`:

- `before/post-lower-types-all.json` records the 13 stale native member selectors.
- The four successful `*-candidate/post-lower-types-all.json` files match their
  `oracle/*.sfc.json` counterparts exactly: 52 member references, including all
  three classes in each fixture. All 56 surviving wrapper ports match name,
  width, direction and order. The padded singleton retains six physical
  zero-width native ports until standard CIRCT backend removal; SFC removes
  them earlier. No retained SRAM annotation points to these zero-width ports.
- All three zero-width and all three aggregate-data annotations reject in both
  compilers. Native CLI diagnostics are in `reject-*.log`; Scala's six exact
  rename failures are checked by the executable oracle.
- `goldengate-lower-types-targets` checks 39 port/node/wire member identities,
  data-port name collision widths, vector elements, singleton aggregate members,
  idempotence, temporary
  symbol cleanup, 65 atomic invalid-member rejections, and debug-only metadata
  preservation, alongside its existing channel/trigger/reset/debug tests.

## Immutable Rocket boundary comparison

`compare-boundaries.py` and `golden-memory-boundary-report.json` in the evidence
directory compare the fresh native `rocket-candidate/post-lower-types.mlir`
and its retained JSON with these exact immutable artifacts:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The register file matches depth 31, data width 64, zero-cycle reads, one-cycle
writes, two read-port identities and one write-port identity. All ten
data/address/enable/mask names and widths match SFC RTL; all three native ports
retain Clock fields. The extraction target remains `~FireSim|Rocket>rf`.
There is a raw annotation count mismatch: the native diagnostic handoff retains
two identical MemModel records from embedded plus external annotations, whereas
the fixture JSON contains one. The report records that extra duplicate rather
than treating the whole annotation arrays as equal. SFC LabelSRAMModels selects
annotated memories with a set, so future extraction must create one wrapper
for this identity.

The local native Rocket compiler also emitted RTL and collateral with empty
stderr. Its `post-fame-ready-valid-wrapper.mlir` matches 138 control bindings
and 70 payload bindings in the same immutable SFC RTL, with zero control
differences and 20 previously recorded AXI fields without SFC counterparts.
The full Rocket input contains no SRAM wrapper port classes, so it does not
exercise their singleton refinement; the four fresh wrapper compilations do.

The recorded Rocket configuration retains the register file in the core and
does not exercise optional multicycle SRAM wrapper generation. This change
establishes wrapper annotation normalization, not SRAM model timing or FAME
equivalence. The next narrow step is native annotated-memory extraction with
one wrapper per identity, the matching ground port records, and an executable
SFC wrapper boundary comparison. Manager gates remain harness-owned; supplied
iteration 39 CIRCT replacertl and required Verilator workloads passed before
this change. The SFC UART-bearing baseline and overall port remain outstanding.
