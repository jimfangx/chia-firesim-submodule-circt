# FAME dependency normalization regression

`MidasTransforms.scala` lowers FIRRTL before `FAMETransform.scala` consumes
`CheckCombLoops` connectivity. Conditional assignments must therefore become
muxes, and repeated assignments must resolve by last-connect priority before
channel dependencies are computed. The normal CIRCT compiler calls
`normalizeFAMEInput` at the target-lowering boundary. Generic retained-target
type lowering remains usable on partially connected analysis fixtures.

`goldengate-comb-dependency` checks nested predicates, overwritten connections,
register boundaries, retained register identity, and explicit rejection of
unnormalized conditional/multiple drivers. It uses actual CIRCT FIRRTL
operations and passes.

The October 6, 2026 comparison extracts `Queue1_AXI4BundleW` verbatim from the
immutable compiler fixture:

```
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

Lines 27307–27364 contain the module; lines 27351–27352 conditionally assign
`io.deq.valid` under `io.enq.valid`. A probe wrapper supplies a concrete UInt1
reset and connects the whole interface without changing the extracted module.
The compiler's `--lower-types` import produces the ground MLIR test input.

The corresponding immutable U250 artifact is:

```
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Its lines 58705–58706 show `io_enq_ready = ~maybe_full` and
`io_deq_valid = io_enq_valid | ~empty`. The CIRCT analysis must report exactly
`io_deq_valid <- {io_enq_valid}` and `io_enq_ready <- {}`; `empty` and
`maybe_full` follow registered state. The test also checks the probe's
hierarchical valid dependency. `io_count <- {}` is checked against the
FIRRTL register boundary; this unused port was eliminated from the SFC RTL.

The mutable extraction, before-normalization IR, normalized IR, and comparison
log are under the U250 generated-source directory's
`iteration1-when-dependencies/`. Recheck that input with:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
"$gg_generated/goldengate-circt-build/goldengate-comb-dependency-test" \
  "$gg_generated/iteration1-when-dependencies/before/post-lower-types.mlir" \
  "$gg_generated/iteration1-when-dependencies/normalized-queue.mlir"
```

This comparison establishes the queue's combinational dependency boundary.
Full transformed-Rocket compilation and behavioral gates remain harness-owned.
