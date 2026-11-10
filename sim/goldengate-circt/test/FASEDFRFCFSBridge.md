# Selected FRFCFS functional/timing and control attachment

Iteration 7 adds native attachments for the selected DDR3 memory model. These
are compiler components; the CLI still rejects the selected platform until its
complete compilation path, driver header and AutoILA/platform integration exist.
CHIA's missing complete candidate artifact inventory remains unresolved.

`FASEDFRFCFSBridge.cpp` attaches the composed LLC/DRAM/releaser/monitor after
`FASEDWriteEgress`, closing the timing request and functional response boundaries.
It uses host clock, `fased_tfire` and `fased_model_reset` directly. The FRFCFS
model owns admission, timing and release, so this path bypasses the LatencyPipe
latency, pairing, retirement and programmable request-limit stages. It requires
explicit base capacity 32 and the selected 35/64/4 AXI widths, eight-beat transfer
limit and maxFlight/idReuse pairs 10/1 or 20/2. It exports remaining model ports
without altering their types or direction. Detached preflight commits only new
modules and circuit attributes, preserving earlier module handles and bodies.

`FASEDFRFCFSMMIO.cpp` materializes the distinct 68-word programming/status bank.
Its exact Scala/SFC contracts and finite differential are documented in
[FASEDFRFCFSMMIO.md](FASEDFRFCFSMMIO.md). `FASEDFRFCFSBridgeMMIO.cpp` attaches that
bank to the composed model, functional relaxed-mode input and host AXI response
observations. The bank uses host reset, while model statistics retain their
target-fire/reset schedule. Host clock/reset and response input leaves are read
from outer wrapper arguments; response-ready leaves come from the inner
instance's outputs. The LLC software word `llc_peakMSHRsUsed` maps to the native
model observation `llc_peakMSHRs`. Transaction queue depth retains full-width
software storage but has no selected hardware consumer. Exact word names,
offsets and permissions are validated before attachment.

`ControlAddressDecode.cpp` chooses the early FASED catalog using the explicit
platform capacity attribute. Base 32 requires the typed FRFCFS bank and allocates
512 bytes for 68 words. Absent/base-16 metadata retains the 21-word, 128-byte LBP
catalog. Mixed programming banks and malformed metadata reject before output
mutation. The existing shared MCRFile transport derives its lane count from the
assembled catalog and uses independent seven-bit AW/AR word selectors for the
68-word bank. No new transport implementation is introduced.

Changed compiler/build files are `CMakeLists.txt`, `ControlAddressDecode.h/.cpp`,
and the three new API/source pairs `FASEDFRFCFSBridge`, `FASEDFRFCFSMMIO`, and
`FASEDFRFCFSBridgeMMIO`. New focused tests are
`FASEDFRFCFSBridgeTest.cpp`, `FASEDFRFCFSMMIOTest.cpp`, shared
`FASEDFRFCFSBridgeFixture.h`, and `FASEDFRFCFSIntegrationTest.cpp`. The minimal
SFC adapter is `FASEDFRFCFSMMIODifferential.py`. Scala oracle sources are unchanged.

The development build uses eight compile jobs, within the manager's sixteen-CPU
affinity. The scoped differential uses at most four Verilator compile jobs.
Exact build and checks:

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build \
  --target goldengate-circt goldengate-fased-frfcfs-mmio-test \
  goldengate-fased-frfcfs-bridge-test goldengate-fased-frfcfs-integration-test \
  goldengate-control-address-decode-test goldengate-clock-bridge-control-test -j8
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  --output-on-failure -R '^(goldengate-fased-frfcfs-(mmio|bridge|integration|timing-model)|goldengate-control-address-decode|goldengate-clock-bridge-control)$' -j3
/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-frfcfs-integration-test \
  --fixture /scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-integration.mlir
.conda-env/riscv-tools/bin/firtool --format=mlir --disable-annotation-unknown --verilog \
  /scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-integration.mlir \
  -o /scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-integration.sv
.conda-env/bin/verilator --lint-only --timing -Wno-fatal \
  --top-module GGFASEDBridgeControlWrapper \
  /scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-integration.sv
```

The bridge test checks four legacy/selected wrapper and capacity combinations,
4,096 wiring/reset/stall/retirement cycles each, all exported configuration
leaves, annotation preservation and fifty atomic negative cases. It injects
timing-model boundary values to test wiring and separately steps the native
releaser; it does not simulate a full target. Existing composed-model tests
cover the independent native model. The integration test checks all bank/model
and host-observation routes, unchanged existing module bodies/handles, exact
68-lane control transport, ten atomic attachment rejections, both early global
catalog profiles and eight malformed/mixed-profile rejections. Its zero-driven
old simulator module is syntax scaffolding only, not functional memory evidence.

Initial focused failures exposed missing C++ includes, a bundle-root versus
leaf-alias assertion in the test interpreter, and the native LLC peak-statistic
name difference. These were corrected before final checks. The firtool syntax
command ignores the fixture-only `test.Annotation` class; no hardware verification
is disabled. Failed logs remain available; final results must be distinguished
from those initial attempts.

The main compiler and focused executables build successfully. Seven distinct
focused CTests pass across the initial run and targeted retest: the six listed
above plus `goldengate-f2-compiler-config`. The initial clock/control test takes
357.55 seconds in this unoptimized development build. It is a component regression,
not compiler timing. Final logs are `iteration7-final-build.log`,
`iteration7-ctest.log` (four passing tests and the two superseded failures),
`iteration7-attachment-retest.log` (both corrected tests pass), and
`iteration7-f2-config.log` in the build directory. Firtool and lint logs are
`iteration7-frfcfs-integration-firtool.log` and
`iteration7-frfcfs-integration-lint.log`; both commands exit successfully.
All four golden receipt/manifest/full-RTL identities match iteration 6;
`iteration7-golden-identities.json` also records successful archived pre-MIDAS
and complete SFC artifact hash checks. All 69 current identities recorded by
the MMIO report were rechecked. No Scala oracle source changed.

The fresh MMIO comparison report is
`/scratch/jfx/goldengate-circt-f2-build/iteration7-frfcfs-mmio-final/result.json`.
It binds the exact MegaBoomV3, plain Shuttle and Saturn Shuttle immutable receipts
to one shared literal SFC cone, with 25,017 cycles and 236 fields checked before
and after edges, 107 resets and 100,816 word writes. Independent status cuts,
zero overflow histogram bins, ready assumptions and read-only-access exclusions
are explicit in the report and MMIO document. Rocket is LBP and is not substituted
into this DDR3 comparison. Complete four-target semantics are still missing.

Next install the platform-derived capacity before early allocation in the CLI,
select these FRFCFS components in the complete functional path, emit the exact
68-word driver ABI, and implement AutoILA/platform behavior. Then produce complete
four-target RTL and collateral inventories for CHIA. The acceptance adapters
remain unset until those prerequisites exist. No Jasper LEC/SEC, F2 bitstream or
runtime, or matched compiler-speed claim follows from these component checks.
The development build is unoptimized, and no long CHIA verification job was
duplicated.
