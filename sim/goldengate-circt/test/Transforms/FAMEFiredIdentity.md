# Fired register identity

`FAMETransform.scala:126–140,265–291` creates a module namespace, reserves a
channel's fired suggestion, then allocates the host register name. Channel
objects retain the register reference. A target declaration with that name
must never become host state, even if its type, host controls and reset match.

`ensureFAMEFiredRegisters` now uses CIRCT's namespace allocator over ports and
all named operations. New registers carry `goldengate.fameFiredChannel`.
Creation is idempotent by identity, and fired-state, finishing, input-ready and
output-valid rewrites resolve that same identity after declaration renaming.
Malformed or duplicate identities reject before mutation. The control
rewrites can still read wholly untagged, already transformed legacy models;
creation never adopts an untagged target register by its name.

The collision fixture includes a target `input_fired_0` register with the same
host controls and reset, a node `output_fired`, and wires `output_fired_0` and
`virtualInput_fired_0`. The executable SFC `Namespace` in
`FAMEFiredNamespaceOracle.scala` gives:

```text
IDENTITY input input_fired_1
IDENTITY output output_fired_1_0
IDENTITY virtualInput virtualInput_fired_1
IDENTITY virtualOutput virtualOutput_fired_0
```

The native test agrees, then renames the output register and serializes and
reparses MLIR. All four control rewrites pass 256 token/state combinations,
leave target state intact and reject duplicate identities atomically.

The immutable boundary compared in iteration 9 is:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Its `FireSim` model declares 42 fired registers at lines 146034–146075.
Inputs reset to one at 146417–146433; outputs reset to zero at
146434–146636, all under `posedge hostClock` and `hostReset`. The native test
recreates host state at the saved Rocket model boundary, verifies the IR and
checks 2,688 reset/finishing/handshake cases. All 42 declaration names and reset
values match the golden (17 inputs and 25 outputs). The golden has ordinary
names; collision coverage comes from the additional executable Scala probe.

Comparison logs are in the mutable generated directory
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration9-fired-identity/`:
`sfc-namespace.log`, `circt-identity.log`, `rocket-boundary.log`,
`comparison.log`, and `tests-final.log` (15 focused tests passed). The native test takes the saved boundary
path as its sole optional argument:

```bash
goldengate-fame-fired-state-test circt-ingestion/post-fame-twenty-fifth-output-control.mlir
```

Compile the Scala probe with the FireSim dependency classpath and run
`FAMEFiredNamespaceOracle`; this only executes namespace allocation and does
not launch a FireSim verification gate. Manager compilation, Verilator and
U250 verification remain owned by the harness.

Remaining scope includes clock-enable flag identity and namespace collisions,
broader multiclock and FAME-5 semantics, and the SFC UART-bearing differential
baseline. The next small step is to resolve clock-enable host state by
identity rather than a fixed declaration name.
