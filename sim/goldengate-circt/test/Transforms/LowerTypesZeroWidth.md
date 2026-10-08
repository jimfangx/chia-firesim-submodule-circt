# Zero-width retained annotation targets

`lowerTypesWithRetainedTargets` now applies SFC's zero-width reference deletion
rule to native CIRCT types. Known zero-width leaves are omitted from expanding
DontTouch, host/global-reset and FPGA-debug annotations. Leaves whose width is
inferred as zero are omitted after CIRCT InferWidths/LowerTypes, using their
inner-symbol identities rather than reconstructing names.

Exact FAME channel/model endpoints, nested ready/valid references, and exact
AutoCounter/trigger members reject if their selected value has zero width.
This matches Golden Gate `RTRenamer.exact`: SFC RemoveZeroWidth supplies zero
rename matches, whereas one match is required. Explicit zero-width selections
reject before changing IR. An inferred zero rejects after normalization, without
publishing replacement annotations; temporary inner symbols are cleaned on both
success and failure. Physical zero-width operations remain available to CIRCT's
standard backend until emission.

This concerns annotations entering target normalization. A zero-data-width
ReadyValidChannel elaborated during SimulationMapping can still carry valid
and ready tokens, as tested by `FAMEReadyValidPayloads.md`.

Iteration 39 evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration39-zero-target-lowering/`.

- `goldengate-lower-types-targets` passes its existing identity/collision cases,
  zero-width fanout tests for five annotation classes, 68 explicit exact-target
  rejection cases across ports/nodes/wires/registers, and an inferred-width
  rejection with annotation publication and symbol-cleanup checks.
- `LowerTypesZeroWidthOracle.scala` invokes the production SFC LowFirrtlCompiler
  and Golden Gate annotation classes. All eight surviving DontTouch/host-reset
  annotations match native JSON in class, target and order. All ten exact-target
  probes reject in both compilers, including an inferred-width wire.
- The fresh native Rocket compiler emits RTL and collateral with no stderr.
  Its `compiler-candidate/post-fame-ready-valid-wrapper.mlir` is compared with
  the immutable SFC artifact
  `/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
  `golden-boundary-report.json` records 138 matching ReadyValid control bindings,
  zero control differences, 70 matching payload bindings, and the same 20
  historical AXI payload fields with no reference counterpart. This comparison
  establishes channel binding correspondence, not whole-design equivalence.

The exploratory SFC probe exposed a separate limitation: explicit selectors for
zero-width children of an aggregate node become dangling targets such as
`alias_valid.pad`. The production comparison uses the aggregate node root and
valid port/wire/register selectors. CIRCT omits the absent node leaves rather
than creating dangling retained metadata. That difference needs a consumer-level
oracle check before claiming complete annotation equivalence.

FireSim manager verification remains harness-owned. The supplied iteration 38
feedback passes CIRCT replacertl and all required Verilator Rocket workloads;
it predates this change. The SFC UART-bearing differential baseline and overall
migration remain outstanding. A next narrow compiler step is to probe retained
zero-width CHIRRTL memory data/port annotations through memory lowering.
