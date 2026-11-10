# Complete FAME model graph with an internal ready-valid link

The prepared scalar fixture has two model definitions, two independent model
instances, two rational clock domains (1/2 and 1/3), three target registers,
a forward data/target-valid channel and a reverse target-ready channel. It
passes the actual generalized graph CLI at both the complete transformed-model
boundary and the active simulator transport boundary.

`ChannelAnalysis` accepts a complete handshake pair at each populated end of a
forward channel. An internal link has four handshake references. Every reference
must select a UInt<1> leaf, the valid reference must match an exact data endpoint
(including field ID), and both ready endpoints must belong to the same unique
reverse channel. The graph validates these pairs before rewriting. Four malformed
complete-graph cases reject atomically, preserving input IR and result values.

The Scala oracle invokes unchanged `FAMETransform`, `AddRemainingFanoutAnnotations`
and `DefineAbstractClockGate` on the same prepared FIRRTL and annotations. Native
output is independently emitted, never transformed through SFC. Supporting
comparisons are under
`/scratch/jfx/goldengate-circt-f2-build/iteration36-recovery-production-features/models/result.json`:

- The final Release CLI complete FAME boundary matches unchanged SFC RTL for
  12,288 host cycles with independently stalled input/output channels, rational
  clock tokens held until accepted, explicit target reset tokens and three host
  resets. All host valid/ready and target-valid controls are strict; data is
  compared when host-valid and, for ready-valid payloads, target-valid. Defined
  stalled payloads remain compared. Each input/output completes over 3,300
  transfers. All 22 ordered retained annotations and two gate/name/MFMR contracts
  match. Both emitted boundaries pass syntax/build checks.
- Invalid RV payload perturbation passes; valid payload and sink-ready corruption
  fail the differential comparison.
- The actual queue emitted inside the final Release transported graph matches
  unchanged production `ReadyValidChannel(UInt(8.W))` for 24,000 host cycles,
  including 19,038 forward/reverse token transfers and 16,312 defined deliveries.
  Its target-reset port uses the exact internal transport binding: valid one,
  bits zero. The existing native RV unit also checks all payload shapes,
  internal connection structure, malformed pairs and 80,000 randomized cycles.
- Both RTL comparisons pass again with independent Verilator startup seeds 11
  and 73. Target payload comparisons begin after explicit target-reset warmup;
  queue payload slots remain uninitialized and are checked only when valid.
  These are concrete samples, not an arbitrary symbolic initial-state proof.

The unchanged production SFC `SimWrapper` cannot elaborate an internal RV link:
its `rvOutputPortMap`/`rvInputPortMap` exclude pairs with both ends populated, then
`genReadyValidChannel` looks up that missing pair. The actual `link_fwd`
`NoSuchElementException` is retained. The complete FAME boundary comparison and
separate unchanged transport primitive comparison therefore do **not** establish
full unchanged `SimWrapper` parity. The native internal binding implements the
existing SFC enqueue/dequeue helper protocol and remains explicitly scoped.

No model-local memory, generalized clockless/multi-hub topology, final platform
MMIO/driver ABI, separate Jasper LEC/SEC, or enabled Linux/F2 representative is
established by this fixture. Those required feature acceptance results remain
open. No baseline golden or Scala oracle source was changed.

After generating the documented Scala/CLI products, reproduce the RTL checks:

```
python3 FAMEModelGraphReadyValidCompare.py <evidence-directory>
python3 FAMEInternalReadyValidCompare.py <evidence-directory>
```

Exact Scala, compiler, firtool, Verilator and runtime commands, source/product
hashes and independent startup invocations are retained in the evidence files.
