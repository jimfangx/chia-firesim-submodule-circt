# Compile-time NIC loopback token boundary

`addNICLoopbackTokenQueues` implements the token-transport part of the SFC
`LoopbackNIC` branch in `SimpleNICBridge.scala`. Each widget owns two ten-entry
queues and the seed/forward scheduler. Ten tokens with packet-valid clear are
sent after each host reset, then outgoing flits are forwarded in order. Unlike
normal CPU packet transport, the 74-bit loopback word retains all eight keep
bits. Reset changes queue occupancy and scheduler state; RAM is never cleared.

The separate `GGNICLoopbackTokenQueuesWrapper` exposes only `nic_done` per
widget. Its retained bindings explicitly identify `compile_time_loopback` and
have no normal CPU stream bindings. MAC/rate constants match the SFC branch.
The three pauser fields that the SFC branch leaves undriven remain explicit
invalid values; this diagnostic boundary does not classify or waive them for
formal signoff.

`--add-nic-loopback-tokens` accepts the prepared post-FAME stream boundary and
emits `post-nic-loopback.mlir`, `.sv` and `-all.json`. It is a component
diagnostic, not production selection of `BaseF2Config` plus `LoopbackNIC`.

`NICLoopbackTest.cpp` constructs two independent widget channel sets, checks
queue geometry, annotations, bindings, state independence and atomic rejection,
and optionally writes transformed and input MLIR:

```text
goldengate-nic-loopback-test output.mlir input.mlir
```

`NICLoopbackOracle.scala` elaborates unchanged SFC `HostToNICTokenGenerator`,
Chisel queues and the actual `HostPortIOConnectChannels2Port`. Compile it with
the unchanged `SimpleNIC.scala` and `SimpleNICBridge.scala` sources and the
matching Chisel compiler plugin. It uses F2 parameters but excludes widget
MMIO/CPU stream assembly. `NICLoopbackCompare.py` lowers both boundaries and
compares 12,000 cycle traces and accepted-token scoreboards, with independent
widget stalls and three host-reset intervals. `--native-rtl` exercises the
actual compiler-emitted RTL instead of another lowering of its MLIR.

The comparison checks every handshake and done predicate, seed packet markers,
live packet data/keep/last/order, and defined configuration constants. It excludes
only seed/invalid packet payloads and the three explicitly undriven pauser
fields. Replays with distinct uninitialized RAM seeds retain identical defined
traces. These exclusions do not authorize arbitrary live unwritten data.

Current evidence is under `/scratch/jfx/nic-loopback-20261025/`; the final
actual Release CLI comparison is `cli-comparison/result.json`. Normal two-widget
transport regressions remain separate.

Production integration still needs the SFC one-word done MMIO/header layout,
the allocated but disabled CPU streams, and a compatible driver initialization
path. The current SFC driver unconditionally writes normal-mode configuration
registers absent in the compile-time loopback branch. Runtime `+nic-loopbackN`
is a separate API requiring its own traffic coverage. No enabled representative
semantic/LEC/SEC/F2 Linux packet receipt is established by these component tests.

## One-word control boundary (2026-10-25, iteration 6)

`addNICLoopbackMMIOBank` now consumes each loopback token path's own `done`
predicate and adds its one-word bank, with independent indexed control bindings.
It validates every widget/port before mutation and preserves copied annotation
targets. The register has the exact SFC `genROReg` behavior: an unreset one-bit
sample overridden by MCR writes, with ignored byte strobes and no reset masking.
The shared `mapNICBridgeControl` derives its size from this actual bank; its
singleton adapter has no index registers and aliases all local addresses.

`--add-nic-loopback-control` composes token queues, status banks and local F2
32-bit-address/12-bit-ID AXI transports from the same prepared input as the token
diagnostic. The output filenames remain `post-nic-loopback.*`. This is still a
prepared-boundary diagnostic; production LoopbackNIC selection is pending.

`NICLoopbackControlOracle.scala` elaborates the unchanged complete SFC loopback
widget and records its actual one-register/four-byte layout. Lowering that
complete widget exposes the three unassigned pauser outputs; the original
source and emitted FIRRTL are preserved without edits. To check MMIO without
waiving that failure, a separate fixture invokes unchanged `Widget.genROReg`
and `genCRFile` at the already-compared status boundary. It does not replace the
required complete feature representative or qualify those undriven fields for
formal signoff.

`NICLoopbackControlTest.cpp` checks two independent production banks/AXI paths,
connections to each live token predicate, singleton schema and five atomic
rejections. Its extraction mode accepts actual CLI MLIR and copies emitted
production bank/adapter bodies into a status-input fixture:

```text
goldengate-nic-loopback-control-test emitted.mlir verified.mlir control.mlir
python3 NICLoopbackControlCompare.py --native-mlir control.mlir \
  --sfc-fir sfc/done-control.fir --output-dir comparison
```

The differential checks 100,004 cycles, independent AW/W/AR traffic and B/R
stalls, arbitrary local address aliases, every strobe, status changes during
held responses and resets with pending transactions. It compares all controls
and valid-qualified response payloads; unreset captures are not assumed zero.
Evidence: `/scratch/jfx/nic-loopback-control-20261025/comparison/result.json`.
The full feature remains incomplete: allocated disabled CPU streams, header and
driver initialization, runtime per-widget loopback, separate complete LEC/SEC,
and F2 Linux packet exercises/poweroff are still required.

## Native compile-time-loopback driver contract

The native `--add-nic-loopback-streams` boundary now also emits
`post-nic-loopback.h`. It registers one distinct `simplenic_loopback_t` for each
widget. The header-only driver is owned by
`goldengate-circt/runtime/bridges/simplenic_loopback.h`; the native driver build
must include `goldengate-circt/runtime`. It uses the existing bridge lifecycle
and widget registry API. Compile-time loopback seeds ten tokens and forwards
packets entirely in hardware, so this driver performs no MMIO writes, reads,
stream transfers, shared-memory setup or simulation termination. The actual
done bank and both allocated disabled CPU queues remain in the hardware.

This is an explicit native correction of the unchanged SFC driver constructor
contract. The complete SFC `genHeader` describes a singleton `done` struct with
offset 0 and size 8, but constructs `simplenic_t`, whose unchanged address struct
places `done` at offset 40 with size 48. Its initialization writes five absent
configuration words and pushes tokens into a queue that compile-time loopback
never consumes. Padding that struct or redirecting its writes would change
hardware behavior. The native compiler instead validates the live one-bit done
sample/write/read relation and disabled stream allocations, then emits a
separate free-running driver with no invented global MMIO addresses. Normal NIC
and runtime `+nic-loopbackN` continue to use `simplenic_t` and its duplex ABI.
The native constructor deliberately does not claim equality to the broken SFC
constructor.

`NICLoopbackCPUStreamsTest.cpp` checks immutable header emission and ten atomic
rejections, including absent/wrong bindings, wrong done state/drivers, enabled
CPU adapter endpoints, stream/widget identity errors and wrong actual memory
capacity. `NICLoopbackDriverCompare.py` compiles the unchanged SFC header against
the unchanged shared driver and requires the two ABI assertion failures. It
compiles the native two-widget header against the real bridge/widget headers and
implementations, runs two independent driver registrations through initialization,
2,000 ticks and finish with no host interactions, and rejects a compiled
corruption that introduces an actual MMIO write. The physical simulator alone
is mocked; no CPU stream engine is constructed.

The three unassigned SFC pauser fields remain an explicit whole-widget syntax
failure. Their existence does not justify relaxing live controls or supplying
new values in the reference. Dedicated driver progress and existing token/MMIO
comparisons do not establish full SFC syntax/semantic parity, complete LEC/SEC,
production F2 assembly or checked Linux packet traffic/poweroff.

## Atomic shared control and F2 CPU port (2026-11-01, checkpoint 40)

`compileNICLoopbackControl` now composes the prepared constructor-free NIC-only
post-FAME boundary into a shared 32-bit-address/12-bit-ID control master and a
64-bit-address/512-bit-data/16-bit-ID CPU AXI slave. Its selected stream catalog
supports one or more independent loopback widgets. Every widget retains the
singleton sampled done bank and both allocated 3,072-entry CPU queues, including
the disabled directions. Shared `HasWidgets` allocation includes the actual CPU
count bank; AW, W, AR, R and B use independent two-entry buffers. The emitted
header combines the dedicated free-running loopback drivers with the real
`CPUManagedStreamWidget` constructor and full directional stream descriptors.

`--synthesize-nic-loopback-control` emits `post-nic-loopback-control.mlir`, `.sv`,
`.h`, `-all.json` and `.peer2peer.const.yaml`. This operation clones and verifies
the complete composition before committing. An unsupported companion widget,
prior driver output, malformed port/catalog, duplicate global router or live
pauser consumer fails before caller-visible mutation. The API requires the
three omitted SFC pauser payloads to be demonstrably unused through ordinary
region-free wires, nodes and internal module inputs. Unknown uses, output
monitors, logic/state consumers and region-bearing operations are rejected.
Their ready/valid token handshakes remain fully live and compared. A model using
`usePauser=true` remains unsupported; no pauser constant is invented.

`NICLoopbackCompositionTest.cpp` checks direct and nested one/two-widget
composition and eight atomic rejection cases, including a conditional pauser
consumer. `NICLoopbackCompositionOracle.scala` elaborates two unchanged actual
SFC `SimpleNICBridgeModule`s, `HasWidgets` allocation/control,
`CPUManagedStreamEngine` and `AXI4Buffer` using F2 parameters. The exact original
FIRRTL is retained and still fails lowering because SFC leaves three pauser
outputs unassigned per widget. `NICLoopbackCompositionCompare.py` preserves that
failure, then uses a separately named diagnostic copy adding explicit invalids
only to those dead outputs. This supporting comparison does not establish
unnormalized SFC syntax parity.

The final Release CLI differential checks 60,000 cycles after an initial
synchronous host-reset edge, with three reset intervals and no RAM clearing.
It compares all 30 token handshakes, valid packet data/keep/last/order, ten seed
markers per widget per reset, strict shared-control responses and occupancy,
independent AW/W requests and response stalls, and empty outgoing CPU queues.
Both allocated incoming queues fill completely through the real buffered AXI
slave: 18,432 accepted writes and 22,091 shared MMIO reads across the reset
intervals. The exact peer-map semantics and all four CPU driver stream
parameters match the unchanged SFC outputs. The dedicated driver lifecycle
comparison and its unexpected-MMIO negative control also pass with the complete
emitted header; the original SFC singleton driver ABI assertion failures remain
explicit reference defects.

Evidence is under
`/scratch/jfx/goldengate-circt-f2-build/iteration40-production-integration/nic/`;
`release-reset-comparison/result.json` records the supporting RTL comparison and
`release-driver-comparison/result.json` records driver checks. This selected
composition still requires the complete target/model/clock/memory and F2 shell
integration, enabled Linux packet exercise and poweroff, and separate complete
LEC and SEC. It does not satisfy a feature acceptance receipt. Normal NIC and
runtime per-widget loopback remain separately required representatives.

## Production platform stages (iteration 41)

The ordinary compiler can select compile-time loopback at its existing NIC
stage through `addNICLoopbackPlatformQueues`. This entry point validates that
all three SFC-unspecified pauser payloads are unused, then atomically creates
the loopback token path and its allocated disabled CPU queues. It preserves
other bridge constructors and target/memory/clock owners for the ordinary
shared platform pipeline. `compileNICLoopbackControl` remains the explicitly
selected NIC-only diagnostic.

After the shared CPU engines and independent AXI buffers, the production path
uses `addNICLoopbackMMIOBank` at `GGNICCPUStreamResponseBufferWrapper`. The bank
requires exactly one live nested loopback token owner with ordered per-widget
mode bindings. `prepareNICHeader`, used by the ordinary driver assembler,
validates the live loopback owners and global read/write allocation before
appending `simplenic_loopback_t` constructors to the retained platform header.
Normal NIC constructors and runtime per-widget loopback retain their existing
path.

The singleton `done` bank remains readable and writable. SFC's `genROReg`
name describes its sampled producer, but `Widget.genAndAttachReg` uses
`attach`'s default `ReadWrite` permissions; `MCRIO.bindReg` writes override the
sample in that cycle. Treating this bank as read-only would diverge from the
unchanged Scala oracle.

`NICLoopbackCompositionTest` additionally checks mixed bridge annotation
preservation, one/two-widget nested production queues, buffered MMIO owner
resolution, shared header extension and live-pauser rejection. These are
supporting compiler integration checks. They do not establish enabled-target
Linux activity or separate LEC/SEC acceptance.
