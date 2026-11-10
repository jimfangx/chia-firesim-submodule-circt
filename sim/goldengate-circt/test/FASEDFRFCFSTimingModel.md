# Selected FRFCFS timing-model composition

Iteration 5, 2026-10-21. `materializeFASEDFRFCFSTimingModel` now composes the
native LLC, FRFCFS controller, AXI response releaser and timing/power monitor.
This covers the selected `FirstReadyFCFSModel` boundary between target requests
and functional-memory egress. It does not implement functional target memory,
host/token/MMIO wiring or the selected platform as a whole. The compiler retains
its explicit rejection of `FRFCFS16GBQuadRankLLC4MB` until those contracts are
integrated. This component result must not populate CHIA's complete candidate
artifact lists or count as a four-target semantic pass.

The Scala oracles are `TimingModel.scala`, `FRFCFSModel.scala`, `Util.scala`
(`AXI4Releaser`) and their emitted immutable selected F2 RTL. No Scala source or
golden was changed.

## Native behavior

The composed module connects target AR/AW/W through the LLC to the controller,
returns backend completions through the LLC, and releases functional response
IDs through two one-entry occupancy pipes. The release pipes support replacement
on a consuming cycle, multibeat reads, independent R/B stalls and unconditional
payload forwarding. Functional memory supplies response metadata/data externally;
the releaser does not invent those values or store request metadata.

All four children share target-fire-gated state/reset. The controller owns the
64-bit target cycle. Native target handshake counters supply the controller's
pending AW/W counts; native commands and rank/bank state supply power counters.
Neither oracle state nor oracle ready/valid signals drive native inputs.

The monitor preserves saturating pending counts, 32-bit totals and power-counter
wrap, old-count occupancy sampling and the Scala four-bin histogram behavior.
The fifth declared histogram element is dead in SFC and explicitly zero native;
samples above eight do not increment a bin. Power covers ACT/CASR/CASW per rank
and cycles when the rank is not refreshing and all eight banks can activate.

Constructors support the selected maxFlight 10 (plain Shuttle) and 20
(MegaBoomV3/Saturn). Invalid capacities and collisions with any of fourteen
internal/external dependency symbols reject before mutation. The original circuit
top and annotations remain unchanged. Target protocol assertions preserve INCR
bursts, single-beat FIXED bursts and accepted lengths below eight; they are
enabled only on target fire outside reset.

## Immutable component comparison

Final report:
`/scratch/jfx/goldengate-circt-f2-build/iteration5-timing-model-verified/result.json`.
The report binds all three exact receipts, fixture/tool hashes, comparator and
imported comparison-policy hashes, commands, generated artifacts and assumptions.
The comparator SHA-256 is
`7b17ab106654bde60481f1e4f2d6626887d1734355dc4bdda527d4e98556d018`.

| Exact target | Immutable attempt | Literal 28-module cone SHA-256 |
| --- | --- | --- |
| MegaBoomV3 | `attempt-20261021T133007Z-c3ba73a8` | `f0714ef9ef06baef0e6bf369e240774a66f7e728d93534722d651b17be254f18` |
| Shuttle | `attempt-20261021T133933Z-f1e312ec` | `85216bb7974ff94619c0de7f1aed8bb30d7cd163b66bc1095da03e0b65ad17c2` |
| Saturn Shuttle | `attempt-20261021T134041Z-08d813d4` | `f0714ef9ef06baef0e6bf369e240774a66f7e728d93534722d651b17be254f18` |

Both distinct cones pass 58,712 cycles with 495 equality checks before/after
edges. Each drains 789 reads and 394 writes (3,439 read beats, 1,672 write beats),
including 691 multibeat reads and 330 multibeat writes. Coverage includes 438
hits, 745 misses, 290 dirty evictions, all eight LLC states, all sixteen runtime
window-depth/page-policy phases, 9,638 host stalls, 3,927 R stalls, 1,336 B stalls
and ACT/PRE/CASW/CASR/REF commands (688/111/290/724/132). This composition removes
the standalone LLC test's always-ready response-consumer assumption.

There are 27 observed R-pipe replacements and zero B-pipe replacements. The
composed workload therefore does not claim B replacement coverage; the standalone
releaser IR evaluator independently covers it. An earlier trial matched all
semantic fields but failed an unjustified B-replacement coverage requirement;
it is superseded by the fresh final directory above, which reports actual
coverage without requiring that event.

The comparison uses identical external requests/settings/readiness and an
independent functional response environment. It queues checked egress IDs,
preserves burst lengths and metadata/data under stalls, and consumes responses
only on actual target valid/ready. Traffic has at most eight reads/eight writes,
one outstanding request per channel/ID, ordered W after AW and up to eight-beat
incrementing bursts within blockBits 6/7 cache lines. Settings change after drain
and reset. It does not model actual functional memory contents or constructor
ID reuse two. The existing eight-byte-block SFC refill hang remains documented
in `FASEDLLCModel.md`; this run makes no liveness claim for that corner.

Verilator's zero initial state models programming-time metadata BRAM
initialization and initial unreset round-robin state; runtime reset preserves
RAM. Invalid payloads and unconsumed unreset metadata are ignored. SFC prunes
response Resp/User fields, so native pass-through is checked against external
constants rather than independent golden fields. Dead-eliminated controller/LLC
length fields are not compared; the functional adapter exercises target burst
length. SFC assertions are disabled with `SYNTHESIS`; native protocol assertion
predicates/gating are tested independently. These are finite simulations, not
Jasper LEC or SEC.

## Reproduction and focused tests

From `/scratch/jfx/fsim-circt` (a fresh differential output directory is required):

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build \
  --target goldengate-circt goldengate-fased-frfcfs-timing-model-test \
  goldengate-fased-axi4-releaser-test goldengate-fased-timing-monitor-test -j8
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-fased-(axi4-releaser|timing-monitor|frfcfs-timing-model)$' \
  --output-on-failure -j3
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-(fased-(llc-model|frfcfs-controller)|f2-compiler-config|debug-snapshot-policy)$' \
  --output-on-failure -j2
python3 sims/firesim/sim/goldengate-circt/test/Transforms/FASEDFRFCFSTimingModelDifferential.py \
  --fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-frfcfs-timing-model-test \
  --firtool /scratch/jfx/fsim-circt/.conda-env/riscv-tools/bin/firtool \
  --verilator /scratch/jfx/fsim-circt/.conda-env/bin/verilator \
  --output-dir /scratch/jfx/goldengate-circt-f2-build/iteration5-timing-model-verified \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/megaboomv3/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/shuttle/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/saturn_shuttle/complete.json --jobs 8
```

Compiler and fixtures build successfully; all seven CTests pass. The releaser
IR evaluator covers 32,192 transitions; the monitor covers 24,000 transitions
per capacity, including saturation, simultaneous increment/decrement, old-count
histograms, counter wrap, stalls and reset. Composition tests check native
feedback wiring, both constructors, 10,000 protocol vectors, unchanged top and
atomic rejection. Evidence logs are `iteration5-build-complete.log`,
`iteration5-new-ctest-final.log` and `iteration5-existing-ctest.log` in the build
directory. Tools are firtool 1.75.0 and Verilator 5.022. Builds/replays use eight
jobs within sixteen-core affinity; distinct differential cones run serially.
The development compiler is unoptimized. No compiler-speed claim is made.

All four receipt/manifest/full-RTL and archived pre-MIDAS identities are rechecked
in `/scratch/jfx/goldengate-circt-f2-build/iteration5-golden-identities.json`.
Rocket uses LBP and is not part of this DDR3 component comparison.

## Remaining compiler work

Next propagate the actual platform BaseParams capacity (32 for selected FRFCFS,
16 for legacy LBP), capped by edge maxFlight, through functional structures.
The retained bridge CompleteConfig does not encode BaseParams, so infer neither
the profile nor its MMIO ABI from maxFlight alone. Route host outstanding, AW
queue, ingress credits/issue/order/deadlock and request-limit construction through
one validated profile before mutation. Keep LBP programmable admission words
separate: FRFCFS does not expose them. MegaBoom/Saturn idReuse two still needs a
real host-buffer/deinterleaver implementation audit. Connect functional release,
distinct FRFCFS MMIO/catalog/header and AutoILA/platform wiring afterward.

CHIA still blocks on missing complete Rocket CIRCT artifacts. Full four-target
syntax/semantic comparison, separate Jasper LEC/SEC, F2 runtime and matched
compiler timing remain incomplete. No duplicate long verification was launched;
all implementation subagents were joined.
