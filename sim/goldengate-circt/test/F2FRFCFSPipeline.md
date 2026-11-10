# F2 FRFCFS compiler selection and functional composition

Iteration 8 connects the selected `FRFCFS16GBQuadRankLLC4MB_BaseF2Config`
compiler path in `tools/goldengate.cpp`. It installs explicit base capacity 32
before early allocation and functional queue construction, materializes the
native 68-word bank, and attaches the FRFCFS DDR3/LLC model after WriteEgress.
The common MCR/control and host/platform stages follow that attachment.
The LBP path retains explicit base capacity 16 and its six register fragments.
Duplicate memory fragments and unsupported FRFCFS/U250 combinations reject.
The F2 enabled-Print diagnostic path still rejects explicitly.

`test/Support/F2CompilerConfigTest.cmake` checks real option parsing and import,
including both capacity attributes in emitted early diagnostic snapshots. The
small imported circuit deliberately stops at the no-data-channel FAME boundary.
This is an importer/configuration test, not successful target compilation.

`test/Transforms/FASEDFRFCFSPipelineTest.cpp` starts with eleven typed channel
endpoints and a constant-output target/channel-fabric scaffold. It invokes the
production TokenEngine, HostOutstanding, AW/W/AR ingress, credits, order, issue,
deadlock, ReadBuffer, ReadScheduler, WriteEgress, FRFCFS bridge, MMIO attachment
and MCR/control mapping in CLI order for maxFlight/idReuse 10/1 and 20/2.
It verifies each stage, stable early bank identity, request capacity, AW queue
10/20 entries, read buffer 128/256 entries, closed configuration/readiness
inputs, the 68-word registry and 512-byte allocation.

The scaffold supplies only target/channel values; all FASED functional and
timing stages are production implementations. This checks compiler composition
and emitted hardware syntax. It does not compare target workloads or establish
whole-target equivalence, memory behavior parity, or platform composition.

Focused reproduction (from repository root; build directory configured already):

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-build --parallel 8 \
  --target goldengate-circt goldengate-fased-frfcfs-pipeline-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  --output-on-failure -R '^goldengate-(fased-frfcfs-pipeline|f2-compiler-config)$'
/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-frfcfs-pipeline-test \
  --fixture 20 /scratch/jfx/goldengate-circt-f2-build/pipeline.mlir
.conda-env/riscv-tools/bin/firtool --format=mlir --disable-annotation-unknown \
  --verilog /scratch/jfx/goldengate-circt-f2-build/pipeline.mlir \
  -o /scratch/jfx/goldengate-circt-f2-build/pipeline.sv
.conda-env/bin/verilator --lint-only --timing -Wno-fatal \
  --top-module GGFASEDBridgeControlWrapper \
  /scratch/jfx/goldengate-circt-f2-build/pipeline.sv
```

Fixture export clones the circuit and removes only its historical
`rawAnnotations` archive, matching `emitSimulatorRTL`. All attached operation
and port annotations remain. Directly sending that archive to external firtool
replays consumed BridgeIO targets onto aggregate ports and fails LowerTypes;
the initial failed syntax logs are retained. Hardware checks remain enabled.

Evidence under `/scratch/jfx/goldengate-circt-f2-build/` records actual commands,
logs and hashes. `iteration8-ctest.log` contains six passing focused tests;
`iteration8-pipeline-syntax-final.json` records fresh syntax results for both
profiles. See `F2FRFCFSHeader.md` for the separate exact driver ABI comparison.

Complete four-target CIRCT RTL/collateral inventories are still required before
configuring semantic acceptance. The CHIA semantic/Jasper/F2/speed adapters
remain unset; component tests cannot authorize a pass. Actual SFC AutoILA logs
skip ILA creation on all three selected DDR3 targets (zero debug selections).
The scoped read-only audit `iteration8-autoila-xdc-audit.json` records those
logs, source hashes and exact golden XDC contracts; it is not a comparison of
emitted candidate XDC. This development build is unoptimized; no speed claim.
