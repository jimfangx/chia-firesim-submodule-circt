# CSV AutoCounter target-token binding supporting case

`AutoCounterBridgeBinding.fir` and `.anno.json` describe two already tokenized
CSV payloads, each with trigger, global target reset, Accumulate64 and Identity64
leaves. Their explicit constructor order supplies widget indices 0 and 1. Clock
metadata declares domain0 1/1 and domain1 1/3. This fixture begins at the retained
constructor plus post-FAME wire-token boundary; it does not synthesize CSV target
annotations, produce rational clocks or compile a Linux representative.

`bindAutoCounterBridgeHosts` validates the complete ordered host collection,
constructor types, typed singleton operations, field widths, local control ABI,
rational clock metadata and distinct latency-zero outgoing pipe channels before
mutating IR. It creates a wrapper containing the original target and both actual
CSV hosts. An incoming payload is accepted only when every constituent token is
valid and its own host accepts the complete token. Each target channel's ready
matches SFC's `DecoupledHelper.fire(ch.valid)` expression; the empty reverse-token
collection makes from-host ready true. The wrapper retains indexed local AXI
ports, clock metadata, constructor targets and source/sink channel annotations.
Failures preserve existing operation identities and SSA values.

The actual compiler mode is:

```
goldengate-circt AutoCounterBridgeBinding.fir \
  --annotation-file AutoCounterBridgeBinding.anno.json \
  --output-dir output --bind-autocounter-hosts
```

`AutoCounterBridgeBindingTest.cpp` checks both F1 local AXI25/id12 and F2 local
AXI32/id12 interfaces, two-widget identity/clock preservation, annotation sinks,
and malformed binding rejection. `AutoCounterBridgeBindingOracle.scala` calls
the unchanged production `HostPortIOConnectChannels2Port` helper on two real
`AutoCounterBridgeModule` instances under F2 parameters. It generates executable
SFC RTL without changing the Scala oracle. The diagnostic JSON fixture preserves
the actual operation singleton classes explicitly; ordinary standalone
`AutoCounterParameters` JSON export loses those nested hints. The normal typed
pre-MIDAS annotations do preserve them, as described in AutoCounterBridge.README.

`AutoCounterBridgeBindingCompare.py` compares the exact compiler-emitted bound
RTL against that SFC wrapper each cycle. It uses eight independently stalled
payload fields, differing domain token schedules, two local AXI buses, trigger
and independent global reset payloads, host reset, rollover, delayed periods,
full sample queues and stalled AXI responses. It compares ready handshakes, bus
handshakes, response identities and CSV register data. Token payloads are external
stimuli; this focused test does not establish rational clock-generator phase.

Global MMIO address allocation, driver/header assembly and final platform
composition remain separate APIs. Production CSV target synthesis, complete
Print integration and the required feature-enabled semantic/LEC/SEC/F2 Linux
checks remain incomplete. These local tests cannot fill the feature acceptance
receipts or establish compiler parity by themselves.

The CSV binding now also exposes `deriveAutoCounterControlMMIOCatalog` and
`prepareAutoCounterBridgeHeader`. These APIs use the complete ordered bound
host registry and actual bank/MCRFile modules. They validate each named register,
its byte offset and permission, its constructor/event payload, the exposed
local control connection, and rational clock ownership before returning output.
The header emitter takes explicit platform `ControlMMIORegion` objects, checks
region identities, bounds and overlap, and produces the real `autocounter_t`
constructor: read/write `AddressMap`, fixed seven-member MMIO struct, widget
index, ordered counters (including local_cycle), descriptions, event widths,
64-bit accumulator widths, addresses and `ClockInfo`. It preserves input IR and
caller text on failure. It does not create global MMIO dispatch.

`AutoCounterBridgeBindingTest.cpp` emits `<output.mlir>.h` when an output path is
provided. Its two local fixture banks are explicitly allocated at 0 and 64 for
this diagnostic only; production headers must use the complete platform
allocation after its real dispatch is connected. Incorrect host order, bank
size, overlapping regions and corrupt register permissions/offsets reject.
`AutoCounterBridgeBindingOracle.scala` now saves the two unchanged SFC
`Widget.genHeader` fragments at the same fixture bases.
`AutoCounterBridgeHeaderCompare.py` compiles both implementations' fragments
against the unchanged production C++ driver header, intercepts construction to
compare all received metadata, and checks that event-address corruption changes
the result. This provides syntax and normalized semantic ABI comparison without
requiring byte equality of C++ formatting. Feature-enabled Linux execution and
separate LEC/SEC are still required for acceptance.
