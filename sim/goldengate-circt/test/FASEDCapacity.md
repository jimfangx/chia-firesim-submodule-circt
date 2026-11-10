# Default FASED capacity support

The default LatencyPipe model now sizes its read-release, write-release and
timing AW queues from `min(16, axi4Edge.maxFlight)`, matching `BaseConfig` and
`LatencyBandwidthPipe.scala`/`TimingModel.scala`. Previously these three RAMs
always held ten entries. Pointer width and wrap now follow the selected depth;
empty flow-through, full-queue backpressure, targetFire gating, model reset,
release-cycle comparisons and overflow assertions retain their contracts.
Historical helper symbols ending in `10` remain for boundary/exporter
compatibility; the symbols no longer specify storage depth.

The MMIO bank and driver-header validators now accept positive 64-bit
`maxFlight` values instead of requiring exactly ten. The six typed fragments,
21 ordered register names/offsets/access permissions, 128-byte allocation,
live forwarding paths and annotation transfers remain checked. Default
occupancy bins `0,2,4,8` and their allocated fifth lane do not depend on
`maxFlight`, so capacity changes do not change this register ABI.

This implements the **default LatencyPipe** contract. The three selected
FRFCFS16GBQuadRankLLC4MB recipes remain explicitly unsupported. FRFCFS extends
`TimingModel`, not `SplitTransactionModel`; it has a different MMIO interface
without LatencyPipe's `writeMaxReqs`/`readMaxReqs` words. It also needs actual
DDR scheduling, LLC behavior and 32-request functional structures. This change
does not substitute a default timing model for that configuration. Other
recorded-profile guards, including host-memory adapters, still need exact
selected-input validation before claiming whole-compiler capacity support.

Changed implementation files:

- `lib/Transforms/{FASEDReadLatency,FASEDWriteLatency,FASEDTimingAWQueue,FASEDMMIOBank}.cpp`
- `lib/Support/FASEDHeader.cpp`
- Corresponding `test/Transforms/*Test.cpp`, `test/Support/FASEDHeaderTest.cpp`,
  and three focused-test source lists in `CMakeLists.txt`.

Focused validation on 2026-10-21, using CIRCT 1.75.0 and GCC 15.3:

```sh
cmake --build /scratch/jfx/goldengate-circt-f2-build --parallel 6 --target \
  goldengate-fased-read-latency-test goldengate-fased-write-latency-test \
  goldengate-fased-timing-aw-queue-test
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build --output-on-failure -j 3 \
  -R '^goldengate-fased-(read-latency|write-latency|timing-aw-queue)$'
cmake --build /scratch/jfx/goldengate-circt-f2-build --parallel 4 --target \
  goldengate-fased-mmio-bank-test goldengate-fased-header-test
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build --output-on-failure \
  -R '^goldengate-fased-(mmio-bank|header)$'
```

All five tests passed. The three queues cover 902,588 interpreted transitions
across maxFlight 1/3/10/16/32 and both default/selected-Print wrapper forms:
depth one, non-power-of-two and power-of-two wrap, capped capacity, full/empty
behavior, stalls, reset and overflow diagnostics. They include 192 wrapper
mapping cases, 30 annotation transfers and 136 atomic rejections. The queue
CTest run took 72.36 seconds; its log is
`/scratch/jfx/goldengate-circt-f2-build/fased-capacity-ctest.log`.

The MMIO/header tests took 1.30 seconds. They check eight maxFlight values
(1/2/8/9/10/16/32/64), three fragment lane orders in both wrapper forms,
21 lanes, six strobe routes and 59 annotation targets per MMIO variant.
They also check 194 MMIO and 45 header atomic rejections, and compare the
complete driver snippet for identical ABI across capacity changes.

No new SFC golden comparison was run for this slice. No immutable SFC files or
Scala oracle sources were changed. These tests are not evidence of any of the
four exact F2 semantic comparisons, Jasper LEC/SEC, F2 runtime or compiler speed.

## Host-memory stages (iteration 3)

Eight subsequent stages now accept positive signed 64-bit `axi4Edge.maxFlight`
instead of requiring ten: `FASEDReadDeinterleaver`, `FASEDHostMemoryBuffer`,
`HostMemoryWriteArbiter`, `HostMemoryWriteResponses`, `HostMemoryReadArbiter`,
`HostMemoryReadResponses`, `HostMemoryOutputBuffer` and `HostMemoryPort`.
Each still requires target geometry 35/64/4, `maxReadTransfer = 8`, and
`idReuse = 1`. Missing, noninteger, nonpositive and wider-than-64-bit attributes
reject before mutation; width, transfer and reuse fields also validate integer
width before extracting their value.

The Scala oracle separates total target flight from per-ID host flight:
`FASEDMemoryTimingModel.scala` `BaseConfig` (lines 153–180) caps total requests
by `axi4Edge.maxFlight`, whereas `maxReadsPerID`/`maxWritesPerID` follow
`idReuse`. The host master (lines 234–245) advertises the per-ID limit, which is
one for the retained profile. `FPGATop.scala` (lines 223–253) composes the
default depth-two AXI4 buffers, LoadMem/FASED crossbar and 64-byte deinterleaver.
Those structures do not depend on the target-total limit. The deinterleaver
retains sixteen queues of eight 64-bit beats, and write-source ordering retains
its depth-two queue. No queue depth or arbitration logic changes in this slice.

The target constructor's 35/64/4 geometry is distinct from the physical host
port. Address translation changes address width 35 to 34; the crossbar adds a
source bit to the four-bit FASED ID; the final adapter extends IDs from five to
sixteen bits. Both U250 and F2 have physical 34/64/16 memory geometry
(`midas/Config.scala`, F1/F2 `HostMemChannelKey`). Changing the constructor
widths to 34/64/16 would conflate these boundaries. The recorded single
`MainMemory_0` region and four-address-set translation are still restricted and
need validation against exact selected F2 inputs.

The eight existing tests now compare the entire transformed circuit, including
interfaces and annotations, for maxFlight 1/10/16/32/INT64_MAX after normalizing
only that constructor field. Their generated hardware must be identical, so
each test's existing interpreted semantic trace exercises the common hardware
once. A shared test helper adds 48 malformed-attribute atomic rejection cases
per stage (384 total), covering all six guarded integer fields.

Focused commands:

```sh
cmake --build /scratch/jfx/goldengate-circt-f2-build --parallel 6 --target \
  goldengate-fased-read-deinterleaver-test goldengate-fased-host-memory-buffer-test \
  goldengate-host-memory-write-arbiter-test goldengate-host-memory-write-responses-test \
  goldengate-host-memory-read-arbiter-test goldengate-host-memory-read-responses-test \
  goldengate-host-memory-output-buffer-test goldengate-host-memory-port-test
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build --output-on-failure -j 3 \
  -R '^goldengate-(fased-(read-deinterleaver|host-memory-buffer)|host-memory-(write-arbiter|write-responses|read-arbiter|read-responses|output-buffer|port))$'
```

All eight tests passed in 99.08 seconds on 2026-10-21. Existing semantics include
12,001-cycle read/write arbiter traces, two 6,000-cycle five-channel buffer
traces, 8,364 deinterleaver edges, and 262,144 platform-adapter cases covering
all 65,536 response IDs and all four handshake combinations (9,699,328 leaf
checks). `git -C sims/firesim diff --check` also passed.

Build output is in `host-memory-capacity-build.log`; test summary and detailed
output are in `host-memory-capacity-ctest.log` and
`host-memory-capacity-details.log` in that build directory. This slice runs no
new immutable SFC comparisons or exact selected F2 acceptance jobs.

## Selected functional capacity and ID reuse (iteration 6)

The shared functional profile now accepts explicit integer circuit metadata
`goldengate.fasedBaseCapacity = 16` or `32`; absence retains base 16. The result
is `min(base, axi4Edge.maxFlight)` with `ceil(log2(limit+1))` count bits. The
retained CompleteConfig constructor carries the target edge, not platform
BaseParams. Never infer the timing model or base capacity from maxFlight alone.
The selected platform handoff must provide base 32 for FRFCFS. The selected CLI
still rejects FRFCFS before running its pipeline and does not yet install that
attribute; this change establishes the functional pass contract for integration.

Six passes now consume the shared profile before mutation:
`FASEDHostOutstanding`, `FASEDIngressAWQueue`, `FASEDIngressCredits`,
`FASEDIngressOrder`, `FASEDIngressIssue` and `FASEDIngressDeadlock`. Their
constructor-specific counter widths, RAM depths and non-power-of-two pointer
wrap now permit effective capacity 20. Existing shared-profile consumers retain
the same validation. Default LBP remains capped at 16. The LBP request-limit
MMIO bank explicitly rejects base 32, even when capped to ten: FRFCFS does not
have its programmable read/write maximum words. This does not install an LBP
bank into FRFCFS or change its ABI.

Eight downstream host stages now accept the selected idReuse values one and
two. Their geometry and hardware remain unchanged. A read-only audit of Scala
and immutable RTL establishes that the deinterleaver has eight beats per ID,
independent of per-ID flight; AXI buffers, source ordering, crossbar and physical
port geometry likewise do not change. These are distinct from read-egress
storage, whose existing native depth correctly changes from eight to sixteen
beats per ID, and write-egress acknowledgement counters, whose existing width
changes from one to two bits. Selected constructors do not activate the read
scheduler's optional ROB branch. Unsupported reuse values still reject.

### Focused checks and immutable comparisons

The compiler and sixteen test executables build with eight jobs. Eighteen CTests
cover the six functional passes, shared profile, legacy LBP request-limit bank,
eight host stages, platform guard and debug-output policy. The new shared-profile
test covers 48 early/mapped cases and 28 atomic malformed/ambiguous rejections;
six pass tests cover base32 effective10/20/32, both wrapper forms and malformed
profile rejection. Each host-stage test compares entire transformed IR and
annotations across twelve maxFlight/idReuse combinations, normalizing only those
two constructor attributes, then exercises its existing interpreted behavior.
Each also checks 51 malformed/unsupported profiles for atomic rejection.

Build/test logs are
`/scratch/jfx/goldengate-circt-f2-build/iteration6-build-complete.log` and
`/scratch/jfx/goldengate-circt-f2-build/iteration6-ctest.log`. From the repository:

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build -j8 --target \
  goldengate-circt goldengate-fased-request-profile-test \
  goldengate-fased-request-limits-test goldengate-fased-host-outstanding-test \
  goldengate-fased-ingress-aw-test goldengate-fased-ingress-credits-test \
  goldengate-fased-ingress-order-test goldengate-fased-ingress-issue-test \
  goldengate-fased-ingress-deadlock-test goldengate-fased-read-deinterleaver-test \
  goldengate-fased-host-memory-buffer-test goldengate-host-memory-write-arbiter-test \
  goldengate-host-memory-write-responses-test goldengate-host-memory-read-arbiter-test \
  goldengate-host-memory-read-responses-test goldengate-host-memory-output-buffer-test \
  goldengate-host-memory-port-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-(fased-(request-profile|request-limits|host-outstanding|ingress-aw|ingress-credits|ingress-order|ingress-issue|ingress-deadlock|read-deinterleaver|host-memory-buffer)|host-memory-(write-arbiter|write-responses|read-arbiter|read-responses|output-buffer|port)|f2-compiler-config|debug-snapshot-policy)$' \
  --output-on-failure -j3
```

The initial run passed seventeen tests and exposed an old capacity-sixteen
assumption in the ingress-order test interpreter: its RAM geometry check and
four-bit addresses rejected/truncated the larger banks. The native two-bank
queue sizing matches Scala. The test now checks exact depth and derives read,
write and expected-memory address widths from that depth. After rebuilding,
only that test and the strengthened mapped-constructor authority test were
rerun. Both passed (152.16 seconds total), completing all eighteen distinct
focused tests. The rerun log is `iteration6-ctest-retest.log`:

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build -j8 --target \
  goldengate-fased-ingress-order-test goldengate-fased-request-profile-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-fased-(ingress-order|request-profile)$' \
  --output-on-failure -j2
```

The actual host-outstanding and AW FIFO helpers also pass a fresh RTL comparison
against exact selected immutable SFC instances. All four targets bind to their
receipts and archived inputs; only identical literal cones share execution.

| Target | Effective capacity | Literal counter/FIFO cone SHA-256 | Compared cycles |
| --- | --- | --- | --- |
| Rocket | 10 | `84946100542f5e3d1c5ef0a4124c0919eb0b59424ecc9f34bb0088b955a54e88` | 30,239 |
| MegaBoomV3 | 20 | `60b0493f531dc182491f7f60faaa3055bafc0a61ca16b4a46ded796bea86a845` | 30,469 |
| Shuttle | 10 | `42a36ac8e718ffce5d48fdc450268570030eea2f3ca971de22c7fd680e3a7790` | 30,239 |
| Saturn Shuttle | 20 | Same literal cone as MegaBoomV3 | Shared execution, exact receipt binding |

The three distinct two-module cones cover 90,947 cycles with 15 fields checked
before/after edges: read/write counts, idle/read-inflight status, FIFO ready/valid
and nine live AW payload fields. Directed traffic fills beyond sixteen, reaches
and holds twenty, repeatedly wraps, stalls, resets and pushes/pops concurrently.
The 20-entry run has 24,393 pushes, 23,530 pops, 18,820 simultaneous push/pop
cycles, 5,990 stalled cycles and 120 reset cycles. Resets deliberately discard
queued entries; these counts are not a workload completion claim.

The test audits actual parent host-counter bindings and ingress queue depth.
Counters receive independent unit handshake fires; SFC's parent has R/B ready
optimized true, while the isolated counter stimulus includes ready in response
decrement fires for unit stress. Queue payload is compared only when valid;
SFC-pruned region/user fields are excluded. Reset flushes queue pointers while
preserving RAM and permitting pre-edge writes. Verilator starts unreset state
and RAM at zero; valid payloads are explicitly written. SFC assertions are
disabled with SYNTHESIS. No SFC internal state drives native inputs.

Final report:
`/scratch/jfx/goldengate-circt-f2-build/iteration6-functional-capacity-verified/result.json`.
It pins comparator, native policy, test/fixture/tool hashes, commands, artifacts
and all 57 recorded immutable receipt/manifest/full-RTL/pre-MIDAS identities,
rechecked before/after. Comparator SHA-256:
`6f1f42eb58cc8aba6112516f498d760471d1415e513ee16d77907a7ebda5f223`.
Reproduction requires a fresh output directory:

```sh
python3 sims/firesim/sim/goldengate-circt/test/Transforms/FASEDFunctionalCapacityDifferential.py \
  --host-fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-host-outstanding-test \
  --aw-fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-ingress-aw-test \
  --firtool /scratch/jfx/fsim-circt/.conda-env/riscv-tools/bin/firtool \
  --verilator /scratch/jfx/fsim-circt/.conda-env/bin/verilator \
  --output-dir /scratch/jfx/goldengate-circt-f2-build/iteration6-functional-capacity-verified \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/rocket/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/megaboomv3/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/shuttle/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/saturn_shuttle/complete.json --jobs 8
python3 sims/firesim/sim/goldengate-circt/test/Transforms/FASEDHostIDReuseAudit.py \
  --repository /scratch/jfx/fsim-circt --goldens /scratch/jfx/f2-sfc-goldens \
  --output /scratch/jfx/goldengate-circt-f2-build/iteration6-host-id-reuse-verified.json
```

The second report is a source/cone audit, not native-vs-SFC behavior proof. It
records exact recipes/constructors and source anchors, immutable identities,
physical 34/64/16 memory geometry and six cones per target. The four relevant
host cones (deinterleaver, input buffer, crossbar and output buffer) are literally
identical among the three DDR3 targets; across all four they match after only
comment/module-name/whitespace normalization. The differing read/write egress
cones are recorded without claiming identity.

Modified implementation: shared `FASEDRequestLimits.h/.cpp`, the six functional
pass sources above, and the eight host sources listed in iteration 3. Tests:
the six functional tests, shared `HostMemoryProfileChecks.h`, new
`FASEDRequestProfileTest.cpp`, `FASEDFunctionalCapacityDifferential.py` and
`FASEDHostIDReuseAudit.py`; CMake adds shared dependencies and the profile test.
Scala and immutable SFC goldens remain untouched.

This evidence excludes complete functional memory composition, host/MMIO ABI,
platform integration, full-target semantics, Jasper LEC/SEC, F2 execution and
compiler-speed acceptance. Tools remain firtool1.75.0/Verilator5.022; the native
development compiler is unoptimized. Distinct cones run serially with eight
compile jobs; concurrent CTests use three jobs within sixteen-core affinity.
No timing claim is made. CHIA still blocks at missing complete Rocket candidate
artifacts, and adapters intentionally remain unset. Next connect the explicit
platform capacity handoff, FRFCFS functional/timing composition and its distinct
MMIO/header/catalog and AutoILA/platform wiring before removing the guard.
