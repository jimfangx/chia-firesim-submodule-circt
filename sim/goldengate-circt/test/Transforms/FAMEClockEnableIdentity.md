# FAME buffered clock identity

`FAMETransformer.hostFlagReg` in `passes/fame/FAMETransform.scala` allocates
each clock enable once through the model namespace. It resets to zero,
captures the incoming clock token on `targetCycleFinishing`, and otherwise
holds. The abstract gate receives `enabled & targetCycleFinishing & !hostReset`.
Outputs use this buffered enable; inputs use the incoming token.

Native `addFAMEClockEnable` now reserves all model ports and named declarations,
allocates a fresh `RegResetOp`, and attaches `goldengate.fameClockEnable` with
the original model clock identity. Gate construction and all four compiler
output-control paths resolve that identity. Untagged target registers are
never reused during creation, even with matching host clock/reset/type.
Read-only lookup permits historical names in entirely untagged transformed
boundaries. A model containing native identities cannot fall back to names.
Malformed and duplicate identities fail before mutation; the gate also checks
the register's host clock, host reset, and zero reset value.

## Local validation

`FAMEClockGateTest.cpp` exercises register/node/port collisions, serialization
after renaming, 32 real/virtual clock control combinations, duplicate creation,
duplicate/malformed identities, invalid host controls, and preservation of
colliding target state. `FAMEClockEnableCollision.fir` exercises the native CLI
and checks that both gate CE and output fired-state consume the generated
enable rather than the same-name target register.

Execute `FAMEClockNamespaceOracle.scala` with the existing Scala FIRRTL
classpath. Its single allocations agree with native names:

```text
IDENTITY target target_enabled_1
IDENTITY other other_enabled_0
```

The Rocket boundary comparison uses the immutable artifact:

```text
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Compare its `FireSim` clock-enable declaration at line 146033, gate I/CE at
146373–146374, and host-clocked reset/update at 146411–146416. Pass the mutable
candidate `circt-ingestion/post-fame-twenty-fifth-output-control.mlir` to
`goldengate-fame-clock-gate-test`. The test replaces its old buffered enable
with a newly generated native register and evaluates the actual mux and gate
operations for all 16 reset/completion/old-enable/token combinations. The
native name, reset, host clock, update priority, and CE truth table match.

Local evidence is retained below the mutable generated-source directory in
`iteration10-clock-identity/{rocket-boundary.log,sfc-namespace.log,comparison.log,tests-final.log}`.
The corresponding immutable `FireSim-generated.implementation.xdc` was inspected:
its buffer output is `clockBridge_clocks_0_buffer/O`, division is one, setup is
one, and hold is zero. This change leaves that gate instance naming unchanged.

## Remaining boundary

Clock gate instance names still require an unused `<clock>_buffer` name, and
`addFAMEClockConstraint` still resolves that name. The next small step is to
unique gate instances through the namespace and propagate their identities
into XDC generation. This change does not establish full multiclock, FAME-5,
or SFC/CIRCT UART workload parity. Manager verification remains harness-owned.
