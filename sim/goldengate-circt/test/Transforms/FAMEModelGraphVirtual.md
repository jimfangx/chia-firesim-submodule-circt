# Hubless virtual-clock model boundaries

`FAMEModelGraphVirtual.fir` has two independently scheduled instances of the
same scalar-clock Worker. Both retain separate target register state and data
handshakes. The unchanged SFC `FAMETransform` accepts this graph without an
explicit TargetClockChannel: each model gets its own VirtualClockChannel.
Previously native `rewriteFAMEModelGraph(..., withTransport=false)` rejected
it with `missing explicit target clock hub`. Boundary mode now composes these
existing virtual clock/control/channel constructors without inventing a hub.
The definition is still shared and target state remains per instance. The result
reports zero explicit hub clock domains. Unknown metadata, clock-shape and
coverage checks still apply and commit remains atomic.

The unchanged SFC `SimWrapper` requires exactly one TargetClockChannel. Native
full transport keeps that restriction, with an explicit diagnostic before any
mutation. The unit tests check retained independent instances and metadata,
one shared definition-local virtual clock gate, removal of stale target top
clocks, valid IR and unchanged IR/result on full-transport rejection. Virtual
groups have no associated local or global clock; assigning the same scalar top
clock to several global groups makes the prepared SFC graph invalid.

Run the actual matched comparison with:

```
python3 FAMEModelGraphVirtualCompare.py EVIDENCE_DIRECTORY GOLDENGATE_BINARY
```

It compiles the test-only `FAMEModelGraphVirtualOracle.scala` against the
immutable SFC jar and invokes the unchanged production FAMETransform,
DefineAbstractClockGate and actual Scala VerilogCompiler. Native receives the
same prepared fixture and annotations. Every retained annotation is compared;
only SFC's inline gate implementation is extracted into a separate Verilog file.
The native source/CLI supplies the gate declaration and links the same actual
SFC registered clock gate. No target state is initialized, forced or cleared.

The 16,384-cycle comparison uses independent startup seeds 11/73, held producer
tokens, independent consumer stalls and three host resets. It compares all
ready/valid controls and actual gated-clock edge schedules after the first
host reset edge. Read-only monitors capture input data at actual target clock
edges. Each instance has its own persistent accumulator ledger which becomes
known only after that instance receives an explicit data-bit target reset.
Both resets must be observed before cycle128; all later valid payloads are
compared strictly, including across host resets. Actual instance-input and
output-ready crosswires must fail. The initial registered-gate startup edge
counter offset is normalized without changing any DUT state.

This supports the prepared FAME boundary API. It does not provide a hubless
production SimWrapper, platform/driver assembly, four-state proof, Jasper
LEC/SEC or feature-enabled Linux/F2 acceptance.
