# DDR3 bank timing primitive

`materializeFASEDBankStateTracker` emits the actual bank-local state machine
used by Scala `BankStateTracker`, not a latency-pipe replacement. It is a public
native FIRRTL materializer for the future FR-FCFS scheduler; the selected
FRFCFS platform remains rejected until its scheduler, rank timing, refresh,
backend, LLC and separate register ABI are connected.

The oracle is `midas/src/main/scala/midas/models/dram/DramCommon.scala`
(`HasDRAMMASConstants`, `DramOrganizationParams`, `BankStateTracker`) and
`models/dram/Util.scala` (`DownCounter`). The selected
`FRFCFS16GBQuadRankLLC4MB` uses eight banks, four ranks and 2^34 bytes.
`DramOrganizationParams.lineBits` defaults to eight, so row storage has
34 − 8 = **26 bits**, independently of the programmable row-address mask.
Its timing inputs retain all 17 Scala fields, including 14-bit `tREFI`,
10-bit `tRFC` and the other seven-bit fields.

The primitive contains active/idle state, unreset open-row storage and three
seven-bit counters governing PRE, ACT and CAS legality. A counter accepts a
set only when its new value is at least its old value. A smaller set falls
through to the normal decrement; it does not hold the old value. Arithmetic
wraps modulo 128, including zero-timing underflow. The commands have Scala's
encodings NOP=0, ACT=1, PRE=2, CASW=3, CASR=4, REF=5. Commands for other banks,
NOP, REF and unused encodings do not change bank state, while counters continue
to decrement. Rank/refresh legality is the caller's responsibility.

The counter set equations below use modulo-128 arithmetic. Unlisted counters
only decrement; the monotonic-set rule still applies to every listed set.

| Selected bank command | PRE counter | ACT counter | CAS counter |
| --- | --- | --- | --- |
| ACT | tRAS − 1 | tRC − 1 | tRCD − tAL − 1 |
| PRE | — | tRP − 1 | — |
| CASR, open page | tRTP + tAL − 1 | — | — |
| CASR, auto PRE | — | tRTP + tAL + tRP − 1 | — |
| CASW, open page | tCWD + tAL + tWR + tCCD − 1 | — | — |
| CASW, auto PRE | — | tCWD + tAL + tWR + tCCD + tRP + 1 | — |

CASR/CASW require active state and an idle CAS counter. PRE requires active
state and an idle PRE counter. ACT requires idle state and an idle ACT counter.
ACT opens `cmdRow`; PRE and CAS with auto PRE close the bank.

State updates and synchronous resets advance only on `targetFire`, matching
the timing model's enabled target clock. Active state and counters reset to
zero. Open-row storage is a Scala `Reg`, not `RegInit`: its initial value is
unspecified, reset does not clear it, and an ACT can write it even during an
enabled reset. Open-row bits are meaningful for row hits only when active.
The four original bank timing assertions are enabled on the relevant command
for this bank when `targetFire && !reset`; illegal commands retain the oracle's
state transitions as well as its assertion failure.

The focused test interprets emitted FIRRTL against an independent command-based
state machine. It covers all eight command encodings, both page policies and
bank selects, default timings, zero/one/max timing boundaries, modular overflow,
smaller sets, random timing/command sequences, reset and stalls, assertion
predicates, and untouched top/metadata plus atomic duplicate-symbol rejection.
It checks all counters and bank outputs each transition. The interpreter seeds
the unspecified initial row to zero in both models and then verifies that reset
retains whichever row was written; that seed is not a hardware reset claim.

```bash
cmake --build /scratch/jfx/goldengate-circt-f2-build \
  --target goldengate-fased-bank-state-tracker-test --parallel 2
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-fased-bank-state-tracker$' --output-on-failure
```

The focused run passes 45,857 transitions, including 11,943 illegal-command
assertion firings, 181 stalled resets, 25 zero-`tRAS` ACT underflow cases and
2,033 smaller-set events. An optional MLIR output argument on the test executable
exports the helper as the circuit root for independent RTL lowering/lint:

```bash
/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-bank-state-tracker-test \
  /scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker.mlir
```

The registered CTest passed 1/1 in 9.97 seconds (9.98 seconds total). Its saved
log is `/scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker-test.log`.
The main native compiler build also includes the materializer. Standalone
lowering with firtool 1.75 and Verilator syntax/lint both exited zero:

```bash
firtool /scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker.mlir \
  --format=mlir -o /scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker.sv
verilator --lint-only --top-module GGFASEDBankStateTracker \
  /scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker.sv
```

The lint log at
`/scratch/jfx/goldengate-circt-f2-build/fased-bank-state-tracker-lint.log` is empty.

This focused source-derived comparison is not an immutable selected-target SFC
cone comparison, four-target semantic pass, formal proof or performance claim.
