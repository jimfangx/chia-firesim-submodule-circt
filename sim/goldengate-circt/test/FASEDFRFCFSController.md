# Native FRFCFS controller checkpoint

The compiler contains an independently composed timing controller for the three
selected DDR3 F2 profiles. `FASEDUnifiedTransactionScheduler`, `FASEDDRAMBackend`
and `FASEDFRFCFSController` each have a header under `include/goldengate/`, an
implementation under `lib/Transforms/`, and a focused C++ test and immutable-SFC
comparison under `test/Transforms/`. CMake builds all implementations into
`goldengate-circt`. The selected platform remains rejected until LLC, functional
memory limits, response release, MMIO and statistics are integrated. These
component results do not establish full-target parity.

The unchanged Scala oracle is `midas/models/dram/TransactionSchedulers.scala`,
`Util.scala` (`DynamicLatencyPipe` and `HasFIFOPointers`), `DramCommon.scala`
(`DRAMBackend`) and `FRFCFSModel.scala`. Paths are relative to
`sim/midas/src/main/scala/`.

The transaction frontend preserves the non-flow, non-pipe eight-entry queue,
two-way round-robin arbitration, write-data completion gating and saturating
completion counter. RR grant state and queue payload are unreset; a reset edge
can still write queue data and update the grant. Selected maximum outstanding
writes are ten for plain Shuttle and twenty for MegaBoomV3/Saturn. Parent pending
AW/W counts are target counts, not reconstructed from LLC memory requests; the
controller zero-extends their four/five bits to Scala's eleven/twenty-one-bit
frontend ports.

The backend preserves independent four-entry read/write queues, twelve-bit
modular deadline equality, off-head deadline expiry, insertion priority over
expiry and no full-queue enqueue bypass on a simultaneous dequeue. Deadline and
payload registers remain unreset. All state and synchronous reset are gated by
`targetFire`; latency assertions use the same enable/reset contract.

The controller wires frontend, address decoding, existing command core and
backend together. It owns the 64-bit target-cycle counter and supplies its low
seven/twelve bits to the command core/backend. It retains the full 32-bit shift
control for 35-bit raw addresses; offsets at or above 35 produce zero. Runtime
rank masks map to enabled ranks as `0 -> 0001`, `1 -> 0011`, `2/3 -> 1111`.
No native scheduler/backend/cycle state is supplied by SFC in the composed replay.

Build and focused checks use `/scratch/jfx/goldengate-circt-f2-build` (an
unoptimized development build, not speed evidence), eight build jobs and at most
eight Verilator build jobs within the sixteen-CPU affinity allocation. The native
backend test checks 42,618 transitions. The frontend tests check 40,065 and 40,075
cycles for capacities ten and twenty. The controller checks both composed IRs,
24,000 decoder vectors including offsets 34/35/63/high-bit values, rank-mask
priority and atomic internal/external collisions for every dependency symbol.
`iteration3-focused-ctest-final.log` records four passing controller/core,
configuration-rejection and snapshot-policy CTests; `iteration3-queues-ctest.log`
records the queue CTests.

Immutable inputs remain the exact attempts listed in
[FASEDFRFCFSCommandScheduler.md](FASEDFRFCFSCommandScheduler.md). All four golden
receipts/manifests/full SFC RTL hashes, including Rocket, were rechecked unchanged.
Differential scripts validate receipt/manifest/RTL identities before and after
simulation, copy literal dependency modules without rewriting SFC, and require
successful process exit plus the target PASS marker. Reports retain exact
commands, tools, comparison policy, generated testbench/RTL and their hashes.

| Comparison | Final evidence beneath the build directory | Scope |
| --- | --- | --- |
| Backend | `iteration3-backend-{megaboomv3,shuttle,saturn_shuttle}-diff/result.json` | Each 56,061 cycles; deadlines/wrap/skipped cycles, full-plus-pop, stalls/reset and handshakes/valid IDs |
| Frontend | `iteration3-frontend-diff/result.json` | Three immutable bindings, two distinct literal cones; 60,114/60,104 cycles for capacities twenty/ten |
| Composed controller | `iteration3-controller-legal/result.json` | Three immutable bindings, two full timing-model dependency cones; 54,007 cycles per cone, raw LLC boundary replay, independently computed native feedback |

All three backend cones have SHA256
`6fd9404e0dd0f4867a16b9615be4e153577baf041d6addb971ace525c6c3261e`.
The frontend cones have hashes
`c43d42382026380729bccc55b7acbf7dc3a47ea48ddea1a9615ea37e6fd670a6`
(MegaBoom/Saturn) and
`3067b3b2dbc81432fc7bcc23de2388177ebc0fc5475a33963270921dab07c9f6`
(Shuttle). The composed comparison uses the same full SFC model cone hashes
`f0714ef9ef06baef0e6bf369e240774a66f7e728d93534722d651b17be254f18`
and `85216bb7974ff94619c0de7f1aed8bb30d7cd163b66bc1095da03e0b65ad17c2`
as the preceding command-core checkpoint.

The composed test replays only raw LLC AR/AW/W requests, target pending AW/W
counts and LLC completion readiness. It compares native request readiness,
completed metadata, decoded/dequeued references, cycle, commands, reference
window, rank/bank timing state and backend availability against SFC. Its SFC
LLC is programmed to a small legal runtime cache (way/set/block bits 1/2/6,
eight active MSHRs) to exercise collisions and eviction; this is not a full-size
4 MB cache implementation comparison. Raw addresses cover all 35 bits, with
fixed bank/rank/row shifts 6/9/11. Native decoder C++ tests cover other mappings.
The driver allows at most eight reads/four writes with one outstanding per
channel/ID; the functional-egress model queues each issued ID until the actual
response is consumed. Backend latency 1200 then 12 stresses queue fullness;
all sixteen window/page-policy phases, ACT/PRE/CASW/CASR/REF, host stalls,
fire-gated reset and complete drain are required.

Normalizations and assumptions remain explicit: selected SFC dead-field
elimination removes lengths (native transport is independently tested); invalid
payloads and command fields not consumed are ignored; unreset row payload is
compared only for active banks. Verilator initializes the unreset RR state and
LLC metadata BRAM to zero; that is a scoped initial-state correspondence, not
proof over arbitrary initial states. SFC assertions use `SYNTHESIS`; native
assertion predicates/gating are independently checked. Selected SFC write latency
and write completion readiness are one, while generic backend tests cover the
retained dynamic/backpressured interfaces. No force/bind or state injection is
used. Earlier composed attempts with a combinational egress-ID echo are
superseded: that environment did not preserve IDs across releaser stalls.

Next port the actual LLC: synchronous metadata read and programming-time
initialization (not reset clearing), eight MSHRs, miss/resource hazards, dirty
writeback/refill ordering and target-gated replacement LFSR. Then integrate
constructor-specific functional admission/release, complete MMIO/power/statistics
and AutoILA/platform wiring. Complete four-target RTL/collateral inventories
are still missing, so CHIA semantic/Jasper/F2/speed adapters remain blocked.
No separate LEC/SEC, F2 runtime or compiler-time result is claimed here.
