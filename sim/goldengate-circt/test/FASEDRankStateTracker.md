# Selected DDR3 rank timing support

`materializeFASEDRankStateTracker` emits the Scala `RankStateTracker` contract
from `midas/models/dram/DramCommon.scala:470–564` for the selected
`DramOrganizationParams(8, 4, 1 << 34, lineBits = 8)`. It instantiates eight
native bank trackers, retains the 26-bit row address, and adds:

- Seven-bit CAS timing, including commands addressed to another rank.
- Ten-bit ACT/refresh timing with monotonic `DownCounter` sets.
- A four-entry activation-expiry queue with seven-bit absolute timestamps,
  equality dequeue, and the Scala non-flow/non-pipe behavior.
- Fourteen-bit refresh interval counting, disabled refresh, and source-order
  priority of interval expiry over REF clearing and refresh exit over REF entry.
- Target-token gating of all state updates and synchronous reset, unreset row
  and queue payload storage, and all five rank command assertions.

Both helper symbols are checked before mutation. The test covers duplicate
rank/bank combinations, including a rank-only collision before bank creation.
The helper is compiled into the native compiler. It is a reusable building
block; the full FRFCFS platform fragment remains explicitly rejected. Refresh
arbitration, scheduling, backend, LLC, functional capacities and distinct MMIO
still require integration. This change does not accept LatencyPipe behavior
for the selected FRFCFS recipes.

## Scoped golden comparison, 2026-10-21

`Transforms/FASEDRankStateTrackerDifferential.py` verifies an immutable complete
receipt, manifest hash and SFC RTL hash, then copies the exact rank dependency
closure without rewriting the SFC modules. CIRCT emits and lowers the new
helper; Verilator compiles both RTL cones together and compares all 55 output
fields after every edge. SFC clock advancement is gated by `targetFire`; the
native helper implements that enable internally. Unreset rows are initialized
by an ACT under reset. Assertions are disabled in both RTL designs for arbitrary
illegal-command stress; the C++ test separately evaluates the actual five
retained assertion expressions in 25,000 predicate/enable checks.

The final fixture covers 92,947 cycles per selected FRFCFS target. Directed
cases include full-queue enqueue/dequeue, zero-expiry non-flow behavior,
timestamp wrap, refresh/REF priority, stalled reset, disabled refresh through
14-bit wrap, and maximum refresh interval expiry. Random stress includes all
command encodings, bank masks, open/closed-page accesses, full-width timings,
7,481 remote CAS commands, 12,181 stalls and 3,030 local REF commands.

| Exact SFC target | Immutable attempt | Result |
| --- | --- | --- |
| MegaBoomV3 | `attempt-20261021T133007Z-c3ba73a8` | 92,947 cycles, all 55 fields pass |
| Plain Shuttle | `attempt-20261021T133933Z-f1e312ec` | 92,947 cycles, all 55 fields pass |
| Saturn Shuttle | `attempt-20261021T134041Z-08d813d4` | 92,947 cycles, all 55 fields pass |

These attempts are under `/scratch/jfx/f2-sfc-goldens/<target>/`. The extracted
SFC cones have identical SHA-256
`79c41cc4061bc012c3890aa75e7c623bddb0c29638e26e25fb1c828b3a5cd3c5`;
each was nevertheless compiled and compared separately against its receipt.
Their closure consists of `RankStateTracker`, `DownCounter_1`, `DownCounter_3`,
`Queue_19`, and `BankStateTracker`. Rocket uses LatencyPipe, so it has no
corresponding rank-cone comparison. No full-target comparison has passed.

Evidence directories are
`/scratch/jfx/goldengate-circt-f2-build/rank-{megaboomv3,shuttle,saturn_shuttle}-final-diff/`.
Each contains exact commands, source/tool hashes, emitted IR/RTL, extracted SFC
RTL, testbench, build/runtime logs and `result.json`. Reproduce each with:

```sh
python3 test/Transforms/FASEDRankStateTrackerDifferential.py \
  --fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-rank-state-tracker-test \
  --firtool /scratch/jfx/fsim-circt/.conda-env/riscv-tools/bin/firtool \
  --verilator /scratch/jfx/fsim-circt/.conda-env/bin/verilator \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/megaboomv3/complete.json \
  --output-dir <fresh-evidence-directory> --jobs 4
```

The native compiler builds. Five focused CTests pass: rank tracker, bank
tracker, debug snapshot policy, F2 compiler configuration, and annotation-null
round trip. Logs are `iteration4-main-final-build.log` and
`iteration4-final-ctest.log` under the native build directory above.
Scala sources and immutable SFC goldens were not edited. This bounded
differential test is neither a Jasper proof nor full simulator equivalence.

## Compiler output and build costs

A single capped Rocket diagnostic used its exact new F2 pre-MIDAS input and
annotations. It reached FASED host-memory buffering with no rejection, then
timed out at 180.024 seconds without simulator RTL. It wrote 233 partial files
totaling 5,012,381,248 bytes, mostly repeated whole-design MLIR. Evidence is
`/scratch/jfx/rocket-native-diagnostic-20261021T143550Z/`. This is an incomplete
compile, not correctness or speed evidence.

Normal `--compile-baseline` now skips 148 optional MLIR serializations;
`--emit-debug-snapshots` restores them. Diagnostic and stop-after modes retain
their snapshots. All passes, annotation exports, bridge-extraction FIRRTL,
RTL and required collateral remain enabled. The policy CTest reaches 13 MLIR
boundaries on a small intentionally incomplete target and verifies identical
failure diagnostics and all 18 emitted non-MLIR boundary files between modes.
It also checks diagnostic and stop-after snapshot retention.

The actual make integration now passes `-DCMAKE_BUILD_TYPE=Release`, overridable
with `GOLDENGATE_CIRCT_BUILD_TYPE`. A make dry-run verifies Release and Debug
selection in `release-make-wiring/{default,debug}.log`. Existing development
binaries were incrementally built with their original unoptimized CMake cache;
no Release compiler performance result is claimed. No passes or required
compiler outputs were removed to produce a timing claim.

All four full-target semantic comparisons, separate Jasper LEC/SEC, F2
bitstream/runtime verification and matched compiler timings remain incomplete.
CHIA adapters remain unset where complete candidate outputs/prerequisites are
missing. Next compiler work is refresh arbitration and the FRFCFS scheduler,
then backend/LLC/MMIO integration and exact-input candidate emission.

## Files changed in this implementation call

Paths below are relative to `sims/firesim/sim/`, except the root port plan:

- `goldengate-circt/include/goldengate/FASEDRankStateTracker.h`
- `goldengate-circt/lib/Transforms/FASEDRankStateTracker.cpp`
- `goldengate-circt/tools/goldengate.cpp`
- `goldengate-circt/CMakeLists.txt`
- `make/goldengate.mk`
- `goldengate-circt/test/Transforms/FASEDRankStateTrackerTest.cpp`
- `goldengate-circt/test/Transforms/FASEDRankStateTrackerDifferential.py`
- `goldengate-circt/test/Support/DebugSnapshotPolicyTest.cmake`
- `goldengate-circt/test/Support/F2CompilerConfigTest.cmake`
- `goldengate-circt/test/FASEDRankStateTracker.md`
- `FIRESIM_CIRCT_PORT_PLAN.md` (repository root; appended checkpoint only)

The final compiler rebuild passes; the three CLI CTests were repeated after
its last diagnostic-message edit and pass in `iteration4-final-cli-recheck.log`.
All delegated agents completed before the main implementation call returned.
