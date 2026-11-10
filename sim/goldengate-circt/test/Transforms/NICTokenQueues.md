# Multiple NIC token queues

`addNICTokenQueues` validates all NIC BridgeIO records and their fifteen channel
bindings before changing the circuit. Each widget receives independent host
controls, completion logic and two queue instances. Widgets may use different
target clocks; channels within a widget must share its clock. Aliased channels,
ports or existing queue boundaries fail before mutation.

One-widget names and ports retain their existing ABI. Multiple widgets use
`nic0_`, `nic1_`, etc. on their host interfaces and queue instances. The
`goldengate.nicTokenBindings` array records their indices and channel/bridge
identities after circuit retargeting, consistently with retained BridgeIO
annotations.

`goldengate-nic-token-queues` checks 12,000 randomized single-widget cycles and
12,000 cycles per widget with independent stalls and clocks. Its expression
evaluator rejects accidental references to another widget's host controls. A
late second-widget alias must reject without changing the original IR. Active
circuit BridgeIO targets must match the retargeted binding metadata.

`NICTokenQueuesCompare.py` additionally compares emitted queue RTL with the
immutable historical SFC `Queue_17`/`Queue_18` definitions and an independent
40,000-cycle scoreboard, including full, empty, simultaneous push/pop and reset.
That historical U250 comparison supports the shared queue primitive only.

Full multiple-NIC compilation remains incomplete: packet adapters, CPU stream
indexing, MMIO, headers and platform assembly still require integration. Normal,
multiple-widget, compile-time loopback and per-widget runtime loopback each need
the enabled F2 representative checks listed in `feature_scope.yaml`, including
separate Jasper LEC/SEC and checked Linux packet traffic through poweroff. These
focused tests grant no feature-acceptance receipt.
