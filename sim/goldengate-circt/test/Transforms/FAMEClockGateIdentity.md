# FAME gate namespace and XDC identity

The Scala oracle in `passes/fame/FAMETransform.scala:193–206` allocates each
buffer once through `Namespace(m)` and constructs its XDC reference from the
allocated instance's output `O`. Native `addFAMEClockGate` now reserves model
ports and named declarations, allocates a fresh `InstanceOp`, and attaches
`goldengate.fameClockGate` with the original model clock identity.

`addFAMEClockConstraint` resolves that identity before attaching the generated
clock metadata to the actual gate operation. This prevents a colliding target
instance from receiving host constraints. Entirely untagged historical
boundaries retain read-only name lookup. Once native identities exist, missing
identities cannot fall back to target names. Duplicate creation, ambiguous
instance names, malformed identities and incompatible port schemas fail before
mutation. Channelized token sources and replacement reads are also preflighted
before creating either the shared gate definition or its instance.

The XDC emitter already follows the operation's current name and hierarchy;
its metadata survives instance renaming, serialization and Xilinx BUFGCE
specialization. Constraint attachment occurs before FPGA specialization.

## Oracle comparisons and local checks

`FAMEClockGateNamespaceOracle.scala` executes the installed Scala FIRRTL
namespace allocator. The three allocated names match the native operations:

```text
GATE target target_buffer_1
GATE other other_buffer_0
GATE third third_buffer_0
```

`FAMEClockGateIdentityTest.cpp` covers collisions with ports, wires, nodes and
an existing abstract gate instance; 24 CE combinations across three clocks;
serialization after renaming; actual target clock use replacement; XDC
attachment and BUFGCE specialization; eight invalid identity/schema cases and
two invalid token cases with unchanged IR. `FAMEClockGateCollision.fir` and its
CLI check exercise the compiler's virtual-clock output-control path with a
colliding buffer wire/node and eight additional CE combinations.

The immutable U250 reference directory is:

```text
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/
```

Compare `FireSim-generated.sv:146150–146153,146373–146374` for the BUFGCE
instance, I/CE/O schema, host clock input and
`clockBridge_clocks_0_enabled & targetCycleFinishing & ~hostReset` enable.
Compare `FireSim-generated.implementation.xdc:3–5` for the actual `O` pin,
clock name, source, divide-by-one and setup/hold MFMR values.

Pass those two files and the mutable
`circt-ingestion/post-fame-twenty-fifth-output-control.mlir` to
`goldengate-fame-clock-gate-identity-test`. The test regenerates the abstract
gate in the actual Rocket `FireSim` module through native operations,
preserves its target clock use and the raw enable token, and checks eight CE
combinations. This boundary precedes simulator wrapper insertion: the test
supplies the recorded final wrapper prefix and renames the existing model
instance to its final `FireSim_` name before resolving XDC. All three generated
clock commands match the immutable reference as an unordered set of commands.
This compares the gate boundary and collateral; it does not regenerate the
whole simulator or establish arbitrary wrapper mapping equivalence.

Evidence is retained in the mutable generated-source directory under
`iteration11-gate-identity/`: `build-cli.log`, `sfc-namespace.log`,
`rocket-boundary-final.log` and `tests-final.log`. The native compiler and
focused clock, fired-state, XDC and Xilinx tests build; all 21 focused tests
pass. FireSim manager gates remain harness-owned.

## Remaining boundary

The baseline hub path still requires one analyzed target clock. The next small
porting step is a two-clock hub channel fixture and per-domain token/gate/XDC
construction, retaining independent buffered enables and incoming tokens.
FAME-5 and the SFC UART-bearing workload differential remain incomplete.
