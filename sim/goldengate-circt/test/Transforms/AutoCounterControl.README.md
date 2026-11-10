# CSV global control assembly

`assembleAutoCounterControl` consumes the complete constructor-ordered collection
from `bindAutoCounterBridgeHosts` and an explicit complete allocation. It rejects
omitted, duplicate, overlapping, incorrectly sized or unrelated/unimplemented
regions. The actual banks determine register sizes and permissions. A caller may
relocate or reorder the explicit regions; widget identity selects each slave.

The transform composes the existing error responder, address decoder, AW route
queue, AW/W dispatch, widget binding, AR dispatch, per-ID read tracker, read and
write response arbiters, write tracker and master interface. Every actual request
and response is connected internally; one `ctrl` AXI bus remains. Generic staged
monitor outputs are removed from the final outer wrapper after checking they are
output-only sink connections. Inner state and nets remain intact. The header is
prepared from the exact supplied allocation before staging and published only
after the whole network verifies. Clone/verify/commit preserves the input IR,
child handles and caller header on failure.

`compileAutoCounterCSVToControl` adds this network to the existing real source →
CSV synthesis → trigger/global-reset/clock wiring → model preparation → FAME →
constructor-host pipeline. It applies the unchanged `HasWidgets` power-of-two
size rounding and stable decreasing-size allocation to the actual CSV catalog.
Both API and CLI modes remain bounded CSV-only integration boundaries; they do
not register the other required platform widgets or implement the complete
Print path.

The focused unit checks F1/F2 address widths, a high relocated allocation and
reversed slave order, and malformed/late-stage failures without mutation.
`AutoCounterControlOracle.scala` invokes unchanged actual CSV widgets,
`HostPortIOConnectChannels2Port`, `HasWidgets.genCtrlIO` and
`genWidgetHeaders`. `AutoCounterControlCompare.py` compares the emitted complete
network using all externally meaningful controls and valid payloads: two stalled
token groups, both clock identities, real counter data, reset/trigger/period
changes, AW/W skew, B/R stalls, repeated outstanding IDs, and unmapped high-address
burst DECERR responses. It checks exact retained constructor/channel metadata.
Unmapped-address printf diagnostics are excluded from cycle records.

`AutoCounterPipelineOracle.scala` and `AutoCounterPipelineCompare.py` also accept
`--global-control` for a source-to-global-control comparison. That case has 17/15
actual words, so the second bank starts at 128 rather than 64. Counter payloads
come from the transformed target. Clock tokens are externally supplied; rational
clock generator phase and a dynamic trigger-source case remain outside this
boundary. Driver header comparison compiles against the actual unchanged
`autocounter.h`/AddressMap/ClockInfo ABI and compares ordered constructors.

No result here creates an enabled-feature acceptance receipt. Separate matched
representative comparisons, Jasper LEC **and** SEC, feature-specific F2 Linux
CSV/Print exercise and clean poweroff are still required.
