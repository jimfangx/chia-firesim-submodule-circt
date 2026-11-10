# Native selected LLC checkpoint

The compiler now builds `FASEDLLCMetadata`, `FASEDLLCMSHRTable` and
`FASEDLLCModel`. Each has a header under `include/goldengate/`, a materializer
under `lib/Transforms/` and a focused test under `test/Transforms/`. The complete
LLC composes metadata, MSHRs, four two-entry request queues, the eight-state
controller, data-array occupancy and statistics. The unchanged semantic oracle
is `sim/midas/src/main/scala/midas/models/dram/LastLevelCache.scala`.

This is the selected eight-way, 4096-set, eight-MSHR geometry, with 35-bit
addresses, four-bit IDs and eight-byte data beats. Runtime controls retain
one/two/four/eight ways, one through eight MSHRs, power-of-two set counts from
one through 4096 and power-of-two block sizes from eight through 128 bytes.
Stored tags
are 32 bits because the selected configuration permits one set; comparisons
retain the full 35-bit shifted address, including truncation on tag writes.
The MSHR enable mask is registered, flight completion overrides allocation,
completion IDs use their low three bits, and metadata payload is unreset.
The free-running replacement LFSR and all state/reset advance only on
`targetFire`. Queue and metadata writes on reset edges follow SFC behavior.

Metadata reads have one target-cycle latency and hold their output between
lookups. A plain latency-one FIRRTL memory did not preserve that contract:
firtool emitted an enable register that invalidated its output on a host stall.
The implementation therefore uses an asynchronous memory read with explicit
unreset output registers enabled by `targetFire && lookupStart`. Simultaneous
read/write captures old data. Runtime reset preserves the RAM; the Scala model
relies on programming-time zero initialization. That initialization remains a
full-platform integration obligation, rather than a reset-clearing behavior.

Build and focused verification:

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build \
  --target goldengate-circt goldengate-fased-llc-model-test \
  goldengate-fased-llc-metadata-test goldengate-fased-llc-mshr-table-test -j 8
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-(fased-(llc-metadata|llc-mshr-table|llc-model|frfcfs-controller|frfcfs-command-scheduler)|f2-compiler-config|debug-snapshot-policy)$' \
  --output-on-failure -j 3
```

`iteration4-final-build.log` and `iteration4-final-ctest.log` under that build
directory record a successful compiler build and seven passing CTests. Metadata
tests exercise 18,000 vectors, including all 24 output-register capture/hold
paths, dirty replacement, high tag bits, reset-time writes and stalls. MSHR
tests check 24,017 transitions, including dynamic enables, full tables,
completion priority, high-ID aliases and stalled reset. The composed structural
test checks IR validity, dependency wiring, unreset payload and atomic rejection
of internal/external dependency-symbol collisions. This is an unoptimized
development build; these measurements are not compiler-speed evidence.

The immutable SFC attempts are the exact MegaBoomV3, plain Shuttle and Saturn
Shuttle attempts listed in [FASEDFRFCFSController.md](FASEDFRFCFSController.md).
`FASEDLLCMSHRTableDifferential.py` replays allocation/completion inputs from their
unchanged full SFC timing-model cones. `iteration4-mshr-diff/result.json` passes
all three bindings: 54,007 cycles for each of the two distinct cones, 1,170
accepted references per run and complete drain. Native len/address payloads
remain tested locally because SFC eliminates them from these cones.

`FASEDLLCModelDifferential.py` instead gives the complete native and SFC LLCs the
same independently generated target requests and delayed backing-memory
completions. No SFC internal state supplies native inputs. It validates receipts,
manifests and full RTL hashes before and after simulation, copies literal SFC
dependency modules, and requires successful exit plus `PASS LLC differential`.
Exact tool commands, tool/comparator hashes, generated RTL/testbench hashes and
assumptions are retained in
`/scratch/jfx/goldengate-circt-f2-build/iteration4-llc-model-verified/result.json`.
The three exact targets share the five-module LLC dependency cone SHA256
`3678aa98cb3a8544039ee8f6a21352232d690c5ff307ddafbd3914ae61a97e55`.

```sh
python3 sims/firesim/sim/goldengate-circt/test/Transforms/FASEDLLCModelDifferential.py \
  --fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-llc-model-test \
  --firtool /scratch/jfx/fsim-circt/.conda-env/riscv-tools/bin/firtool \
  --verilator /scratch/jfx/fsim-circt/.conda-env/bin/verilator \
  --output-dir /scratch/jfx/goldengate-circt-f2-build/iteration4-llc-model-verified \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/megaboomv3/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/shuttle/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/saturn_shuttle/complete.json \
  --jobs 8
```

The complete replay compares 107 fields before/after clock edges across 77,131
cycles. All 3,544 reads and 1,273 writes receive responses and drain. It covers
3,772 hits, 1,045 misses, 290 dirty evictions, 998 refills, 154 partial-write
refills, 47 full-write misses and 2,263 multibeat read hits. All eight states,
all way/MSHR settings, set bits 0/1/2/5/8/12 and block bits 3 through 7 are
exercised, including set 4095 and address bit 34. Resource/set hazards, target
stalls and independent backing AR/AW/W backpressure are required.

Scope and assumptions are explicit. Response consumers stay ready: SFC checks
readiness at admission and emits response pulses, so arbitrary response-ready
withdrawal is not covered. Bursts fit the configured block, IDs are not reused
while outstanding, and write beats follow accepted AW order. Verilator starts
unreset registers and metadata RAM at zero; runtime reset preserves metadata.
SFC dead-field elimination removes response/backing-request lengths and other
constants, so absent outputs are not compared. Metadata is checked when consumed
in the metadata-access states. Assertions use `SYNTHESIS`; native assertion
predicates are checked separately. Runtime settings change after drain/reset.

The eight-byte-block read-miss corner reproduces an existing SFC behavior: its
response is delivered but the refill FSM stays in state seven with occupancy
zero. A 700-cycle bounded comparison checks that behavior, followed by a reset;
random draining traffic uses larger blocks. This is not a successful liveness
proof for that corner. The scoped simulation is not Jasper LEC/SEC or full-target
parity.

Next compose LLC and the existing FRFCFS controller with constructor-specific
functional admission/release, MMIO, power/statistics and AutoILA/platform wiring.
The selected DDR3 platform remains explicitly rejected until that integration
is complete. No complete four-target candidate inventories, semantic pass,
Jasper results, F2 runtime or matched compiler-speed results exist yet; the
corresponding CHIA adapters remain blocked. No duplicate long verification jobs
were launched.
