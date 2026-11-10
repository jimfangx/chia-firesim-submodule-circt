# Repeated model definitions with grouped channels

`rewriteFAMEModelGraph` now transforms multiple instances of one model definition
when their input/output channels contain several scalar leaves. It transforms the
shared definition once, checks the shared channel ABI on each subsequent instance,
and rewrites each instance's leaves independently. Source field order, types,
directions and original producer identities must agree. The transformation remains
atomic: incompatible aggregate field order, missing leaves and unsafe annotations
leave both the input IR and result unchanged.

During this rewrite one instance can temporarily retain the old scalar port list
while the shared module already has its aggregate interface. The graph pass reads
only direct instance/top-level edges during this clone-local interval; its original
whole-graph checks and final verifier still run. Leaf identity is resolved from the
live instance until every instance has been converted.

The supporting fixture has two real clock domains with rational 1/2 and 1/3 clock
tokens, two model definitions, three instances, and independent grouped channels
on both instances of the stateful Worker definition. Local channel names follow
the unchanged SFC model deduplication result, so complete transformed Top wiring
matches exactly. `FAMEModelGraphSharedGroupedOracle.scala` invokes the immutable
SFC `FAMETransform`, `AddRemainingFanoutAnnotations` and production
`DefineAbstractClockGate`; it never transforms native output using SFC.

Evidence for 2026-10-25 is in `/scratch/jfx/shared-grouped-models-20261025/result.json`.
It binds inputs, immutable SFC jar, source files, exact commands and emitted outputs.

- Private rebuilt native unit: shared grouped positive case, three atomic negative
  cases and explicit aggregate Pipe transport rejection; original graph unit also
  passes its ten atomic negative cases.
- Actual compiler CLI: two definitions, three instances and two real domains.
  The CLI MLIR is independently exported/parsed. Exact complete Top wiring and
  module interfaces/state signatures match SFC; 576 emitted-model behavior rows
  cover independent stalls and reset/clock schedules.
- Both SFC and native RTL pass firtool emission and Verilator syntax checks.
  Independently emitted actual CLI and SFC complete Top RTL match for 8,192 host
  cycles using the exact production registered `AbstractClockGate` from SFC's
  `BlackBoxInlineAnno`. Rational clock tokens stay stable until accepted. All
  valid/ready controls and valid payloads (including stalled valid payloads) are
  compared with independent stream stalls, target reset tokens and three host
  resets. Each of four input streams completes over 1,900 transfers.
- Twenty-two ordered retained boundary annotations match exactly without name
  normalization. Two generated-clock gate/name/MFMR contracts match native
  structured constraint metadata. Final platform XDC is outside this fixture.

Reproduction uses the checked fixture FIRRTL/annotations and Scala oracle. Exact
Scala classpaths and firtool/CLI arguments are retained in `sfc-command.json`,
`scalac-executed-command.json`, `commands.json`, `actual-cli-compare-commands.json`
and `result.json` in that evidence directory. After producing the documented
`sfc-fame.fir/json`, `sfc.sv` and `native-cli/` products:

```
python3 FAMEModelGraphSharedGroupedAnnotations.py /scratch/jfx/shared-grouped-models-20261025
python3 FAMEModelGraphSharedGroupedRTL.py /scratch/jfx/shared-grouped-models-20261025
```

This is a complete transformed-model boundary comparison, not complete feature
acceptance. SFC's production `SimWrapper.scala` rejects aggregate Pipe channels;
native graph transport now rejects that same unsupported case before mutation.
The fixture therefore does not establish aggregate Pipe transport, ready-valid
transport, model-local memory, MMIO/driver/platform ABI, Jasper LEC/SEC, or a
multi-clock Linux/F2 representative. The actual RTL run samples the same concrete
zero initial target register state in Verilator; the emitted-model interpreter
uses common initial state 37. Neither sample proves an arbitrary initial-state
relation or adds target initialization. There is no RAM in this fixture and no
memory clearing. The outstanding full feature representative must still execute
all required semantic/formal/Linux checks under `feature_scope.yaml`.
