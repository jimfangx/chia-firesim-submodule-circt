# Native FRFCFS command scheduler checkpoint

The native compiler now materializes a selected-organization command scheduler:
four DDR3 ranks, eight banks per rank, an eight-entry collapsing reference window
and refresh arbitration. This is reusable compiler implementation, tested through
emitted RTL. It is not yet connected as the complete FASED timing model. The
`FRFCFS16GBQuadRankLLC4MB` platform guard remains in place.

The Scala oracle remains unchanged:

- `midas/src/main/scala/midas/models/dram/FRFCFSModel.scala:81–275`
- `midas/src/main/scala/midas/models/dram/DramCommon.scala:634–654`
- `midas/src/main/scala/midas/models/dram/Util.scala:217–274`

## Implementation and preserved contracts

New compiler files are `include/goldengate/FASED{RefreshUnit,CollapsingBuffer,
FRFCFSCommandScheduler}.h` and their matching `lib/Transforms/*.cpp` files.
`CMakeLists.txt` compiles all three into `goldengate-circt` and adds three focused
test targets. Each helper preflights its symbols before changing the circuit;
the composed scheduler checks all five helper symbols before creating children.

The scheduler accepts decoded request metadata, target-cycle enable/reset,
backend read/write availability, runtime timings, window size and page policy.
It independently computes window admission, entry readiness, legal oldest CAS,
the oldest ACT/PRE decision, refresh priority, auto-precharge, bank-ready state,
backend request metadata and read/write latency. Four existing rank trackers
carry the complete bank/rank timing state.

Preserved oracle details include:

- Refresh and precharge proposals are independent, masked by ranks in use.
  No-match encoder results are rank three/bank seven, including invalid payloads.
- Runtime window size is three bits: default eight truncates to zero. The
  collapsing window retains the reversed one-hot boundary and shifts updated
  younger entries exactly one position per enabled target cycle.
- Arbitration is REF, refresh PRE, legal CAS, oldest pending ACT/PRE, then NOP.
  The Scala `cmdBusBusy` counter does not gate arbitration. Its unused state and
  instrumentation are outside this command-core boundary.
- The enqueue-cycle command changes incoming readiness. The later incoming-ready
  assignment overrides command-driven `bankHasReadyEntries` updates.
- `tCAS+tAL` wraps at seven bits before adding twelve-bit backend latency.
  State and reset advance only on `targetFire`; request payloads remain unreset.

## Focused verification

Build directory: `/scratch/jfx/goldengate-circt-f2-build`. Development build is
unoptimized; these checks make no compiler-speed claim. Native builds used eight
parallel jobs. Verilator comparisons used at most six; independent component
comparisons were capped to fit the sixteen-CPU affinity mask and available RAM.

Seven CTests pass, recorded in `iteration2-focused-ctest.log`:

```sh
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  --output-on-failure -j 4 \
  -R '^goldengate-(fased-(bank-state-tracker|rank-state-tracker|refresh-unit|collapsing-buffer|frfcfs-command-scheduler)|f2-compiler-config|debug-snapshot-policy)$'
```

The refresh test checks 118,304 arbitration cases, including invalid defaults,
multiple simultaneous proposals and masking. The window test checks 44,779
transitions, all sixteen helper depth encodings, synchronous valid-only reset,
stalls and unreset payload transport. The composed test verifies IR, four-rank
composition, 10,000 latency-wrap cases and atomic internal/external symbol
collisions.

The three Python differential adapters under `test/Transforms/` validate the
complete immutable receipts and manifests before reading SFC RTL. They copy
literal module cones without editing SFC and require target PASS markers and
successful process exit. Exact commands, tool hashes, testbench/RTL hashes and
logs are retained in each report.

All three DDR3 profiles use these exact receipts:

| Target | Immutable SFC attempt under `/scratch/jfx/f2-sfc-goldens/<target>/` |
| --- | --- |
| MegaBoomV3 | `attempt-20261021T133007Z-c3ba73a8` |
| Plain Shuttle | `attempt-20261021T133933Z-f1e312ec` |
| Saturn Shuttle | `attempt-20261021T134041Z-08d813d4` |

Component reports beneath the build directory:

- `iteration2-refresh-diff/result.json`: all three receipts, one identical
  literal refresh cone, 118,304 vectors and all five outputs.
- `collapsing-buffer-{megaboomv3,shuttle,saturn_shuttle}-diff/result.json`:
  32,377 cycles and 81 observable fields per target, every runtime depth, 6,522
  stalled cycles, 309 resets, 30,129 kills and 59,802 updated-younger rewrites.
- `iteration2-command-scoped-final/result.json`: composed scheduler replay
  against the two distinct complete SFC `FirstReadyFCFSModel` dependency cones.
  MegaBoomV3 and Saturn share a byte-identical cone; plain Shuttle is separately
  compiled and checked. The two literal cone hashes are
  `f0714ef9ef06baef0e6bf369e240774a66f7e728d93534722d651b17be254f18`
  and `85216bb7974ff94619c0de7f1aed8bb30d7cd163b66bc1095da03e0b65ad17c2`.
  Both executions pass 51,007 cycles with 1,288 accepted references, 9,645 host
  stalls, all sixteen phase coverage bits set, 1,097 ACT, 459 PRE, 344 CASW,
  944 CASR and 148 REF commands. Final read/write/write-data counts are zero.

The composed replay instantiates the untouched full SFC timing model under AXI
traffic and observes decoded request inputs, backend readiness and target cycle.
Those inputs drive the independent native command core; no native signal feeds
back to SFC. There is no `force`, state injection or modified oracle module. It
checks 313 named output/state fields, conditionally masking only semantically
unused payloads. Each of the sixteen depth/page-policy phases must accept
traffic, all five DRAM command types must occur, and final traffic must drain.

The SFC cone removes `len`; its native transport is checked in the C++ window
test, not claimed as an SFC differential result. Invalid window payloads,
inactive unreset open rows and command-unused rank/bank/row fields are ignored
with explicit report normalization. SFC design assertions are disabled under
`SYNTHESIS`; rank assertion predicates have a separate focused test. SFC LLC
memory starts at zero in Verilator, consistent with its documented programming
initialization. Timing/layout settings and backend latency are fixed in replay;
latency overflow is separately tested by evaluating native IR. This simulation
is neither a Jasper proof nor full-target/compiler parity.

Initial diagnostic attempts failed their coverage requirement: invalid LLC
block settings stalled the environment, and a 97-cycle refresh interval below
the 160-cycle refresh duration starved later requests. Those failed reports are
retained. The final stimulus uses 64-byte lines, default 7,800-cycle refresh then
a 600-cycle interval, legal write-data accounting, read/write/drain subphases,
enabled/stalled resets and host stalls.

## Remaining compiler integration

Next implement the separate four-entry read/write `DynamicLatencyPipe` backend:
twelve-bit modular deadline equality, retained pending bits, latency-one case,
no flow-through and no full-queue bypass. The existing LatencyPipe FASED helpers
have different semantics and cannot substitute. Then connect the round-robin
AR/AW transaction front end, address decoding, LLC, power counters and distinct
MMIO/driver ABI, preserving the actual extracted constructors.

The immutable constructors have effective read/write capacities **20/10/20**
for MegaBoomV3/plain Shuttle/Saturn, with ID reuse **2/1/2**; Rocket has capacity
10 and ID reuse one. The Scala defaults of 32 are capped by the extracted edge
`maxFlight`. All four have 35/64/4 address/data/ID widths and eight-beat maximum
transfers. The new command core does not encode functional capacities.

No complete CIRCT design/collateral inventory exists for the four acceptance
targets. The CHIA semantic node remains blocked at the Rocket artifact list;
semantic/Jasper/F2/speed adapters remain intentionally unset. Full-target
semantics, independent LEC and SEC, F2 bitstreams/runtime and matched compiler
speed are all incomplete. Immutable SFC goldens and unrelated worktree edits
are preserved.
