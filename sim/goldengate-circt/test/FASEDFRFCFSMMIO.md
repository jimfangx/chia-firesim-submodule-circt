# Selected FRFCFS MMIO register bank

The selected MegaBoomV3, plain Shuttle and Saturn Shuttle immutable SFC outputs
have the same 68-word FASED register catalogue. Each word occupies four bytes;
all words are readable and 32 are writable. This is the
`WithAutoILA_FRFCFS16GBQuadRankLLC4MB_BaseF2Config` catalogue, distinct from the
LatencyPipe register fragments. The native materializer and catalogue live in
`include/goldengate/FASEDFRFCFSMMIO.h` and
`lib/Transforms/FASEDFRFCFSMMIO.cpp`. The Scala oracle remains unchanged.

| Word indices | Meaning |
| --- | --- |
| 0–1 | transaction queue depth, scheduler window size |
| 2–17 | Four ranks, each ACT/CASW/CASR/all-precharged cycle counts |
| 18–34 | DRAM timings WTR, WR, RTRS, RTP, RP, RRD, RFC, RCD, RC, REFI, RAS, FAW, CCD, CWD, CMD, CAS, AL |
| 35–42 | Backend latency, open-page policy, row/rank/bank mask and offset pairs |
| 43–52 | Five write and five read outstanding histogram bins |
| 53–60 | LLC peak MSHRs, refills, writebacks, misses, active MSHRs, block/set/way bits |
| 61–64 | Write beats, read beats, writes, reads |
| 65–67 | Relax functional model, R response error, B response error |

Writable indices are 0, 1, 18–42, 57–60 and 65. All writable host registers
retain and read back all 32 bits. Their model consumers use the exact selected
widths: the scheduler takes three bits, for example, so its default host value
eight appears as zero at that consumer. `transactionQueueDepth` remains an
allocated/readable/writable word even though selected SFC eliminates its model
consumer. Programmable defaults and every model truncation are extracted from
actual SFC parent RTL rather than inferred from names or reused LBP defaults.

The bank runs on the host clock. Host reset restores programmable defaults and
clears the two error registers; target-fire and target-reset schedules belong
to the external timing model's status signals. Read readiness does not consume
register state. `MCRIO.bindReg` ignores byte strobes; even a zero strobe writes
the full word. The B response error intentionally retains the oracle behavior:
a nonzero B response samples the R response value. These semantics must not be
silently changed during integration.

`test/Transforms/FASEDFRFCFSMMIODifferential.py` validates each exact immutable
receipt, manifest, archived pre-MIDAS resources, complete SFC RTL and driver
header identity. It parses the actual header read/write address maps and checks
the native fixture's catalogue. It extracts literal register declarations,
reset/write blocks, read bindings and consumer truncations from
`FASEDMemoryTimingModel`, retaining their original expressions and source
comments. The three selected receipts share one literal extracted cone, SHA256
`533dc8bed6d6d602fffc7a5976e6b4bc0b7a6c9a67766a6601ebe51aff327b2c`.
Each target retains its separate recipe and input identities in the report.

The replay compares 236 fields before and after each edge: 68 read words,
136 read-valid/write-ready flags, 31 live configuration projections and the
allocated word-zero storage observation. Directed
traffic writes high bits into every host register and covers all sixteen byte
strobe values; randomized traffic changes host reset, enables, full-width write
data, read readiness, error responses and independently supplied model status.
Simultaneous enables stress the vector bank independently of the external
single-address MCR transport. Read-only writes are excluded as illegal accesses;
the native unit test separately checks their diagnostics.

Two selected SFC histogram overflow bins (words 47 and 52) are optimized to zero
inside the actual MCRFile read decode. Their external status inputs are therefore
held at zero for this comparison. Other read-only status inputs are independent
cuts: this replay does not compare their counters or target-fire schedule. R/B
ready is held true, matching the actual selected SFC parent after optimization.
The native bank accepts explicit ready signals for eventual composition.
All bank state is reset before random traffic; no uninitialized-state equivalence
is claimed. The selected literal blocks are wrapped in a generated module with
exposed write/status inputs and read/configuration outputs. The extraction and
these cuts are the complete comparison policy; the immutable source is not edited.

Run after building `goldengate-fased-frfcfs-mmio-test` in the development build:

```sh
python3 sims/firesim/sim/goldengate-circt/test/Transforms/FASEDFRFCFSMMIODifferential.py \
  --fixture /scratch/jfx/goldengate-circt-f2-build/goldengate-fased-frfcfs-mmio-test \
  --firtool /scratch/jfx/fsim-circt/.conda-env/riscv-tools/bin/firtool \
  --verilator /scratch/jfx/fsim-circt/.conda-env/bin/verilator \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/megaboomv3/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/shuttle/complete.json \
  --golden-receipt /scratch/jfx/f2-sfc-goldens/saturn_shuttle/complete.json \
  --output-dir /scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-mmio-final \
  --jobs 4
```

The output directory must be new. The report records commands, versions and
hashes for the comparator, production materializer/header, fixture source and
executable, FIRRTL lowering and simulation tools, relevant Scala files, exact
selected input identities, native catalogue, extracted snippets, generated
wrapper and simulation logs. It rechecks source/tool/immutable hashes after the
run. Failed commands leave `passed: false` and propagate failure.

This is finite register-component evidence. It does not establish full FASED
functional/timing composition, control-address routing or AXI transport, complete
driver integration, AutoILA/F2 behavior, any target's whole-design semantic
parity, Jasper LEC/SEC, runtime, or a compiler-speed result. Complete selected
candidate outputs remain required before CHIA adapters can be enabled.

Iteration 7 finite replay passes for all three exact selected receipt bindings.
The shared literal cone executes 25,017 cycles, comparing 236 fields before and
after edges. It covers 107 host resets, 100,816 independently enabled word
writes, 1,569 zero-strobe cycles, 21,868 partial-strobe cycles, 1,580 full-strobe
cycles, 850,984 per-word read-stall observations, 15,231 nonzero error-response
cycles and 7,048 B-error captures with differing R/B responses. The source,
fixture, tool and 45 selected immutable input identities remain unchanged.

Final report:
`/scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-mmio-final/result.json`,
SHA256 `3c1ef78d92ee170b890b6d17a2d2f39a1cc6ab6afc945c864959db5ea03f445b`.
Comparator SHA256:
`da5a0247c2d632a30c080fab7c8b29da8fd3a53cf487709e7d9f9fe863a38b88`.
Tools are CIRCT firtool 1.75.0 (LLVM 19.0.0git optimized) and Verilator 5.022.
An initial attempt under `iteration7-frfcfs-mmio-verified` failed during
Verilator compilation because the generated task name `randomize` collided with
SystemVerilog's reserved method name. Renaming that testbench task to
`driveInputs` produced the fresh passing run; production RTL was unchanged.
