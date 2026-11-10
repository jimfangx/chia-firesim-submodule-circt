# Selected F2 FRFCFS driver header

The native header emitter in `lib/Support/FASEDHeader.cpp` now supports the
selected FRFCFS16GBQuadRankLLC4MB profile alongside the existing LatencyPipe
profile. `test/Support/FASEDHeaderTest.cpp` exercises both. Build wiring is in
`CMakeLists.txt`.

An explicit circuit `goldengate.fasedBaseCapacity = 32` selects the FRFCFS ABI:
68 readable words, 32 writable words, a 512-byte allocated region and independent
seven-bit MCR AW/AR word selectors. Absent or integer-sixteen metadata retains
the 21-read/five-write, 128-byte LatencyPipe ABI. Invalid or mixed profiles reject
before modifying annotations. Both legacy and selected-Print wrapper hierarchies
are supported.

The emitter requires unique live token engine, bank, MCRFile, address decoder,
control and allocated-slave binding paths. The FRFCFS constructor must retain
`firesim.lib.bridges.CompleteConfig`, 35/64/4 AXI widths, eight-beat read/write
transfers and maxFlight/idReuse 10/1 or 20/2. It checks the native register table
against the advertised catalogue, exact permissions and offsets, the actual
aggregate-bank connection, bank clock/reset, model programming/status routes,
functional relaxed mode, and functional R/B observations. It also checks the
allocated control request/response routes and address width. A disconnected or
copied catalogue cannot authorize header emission.

The Scala oracle is
`sim/midas/src/main/scala/midas/models/dram/FASEDMemoryTimingModel.scala:591-602`:
`genHeader` retains widget index, `memory_stats<index>.csv` and the target Nasti
address-space exponent. The emitted FRFCFS header includes the exact selected
native MMIO order. The transaction-depth register remains in the driver ABI
even though the selected scheduler uses its elaboration-time FIFO depth.

## Focused validation

The main implementation agent runs builds centrally. The focused executable is
`/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-header-test`; its CTest
name is `goldengate-fased-header`.

The unit fixture uses production native MMIO materialization, FRFCFS bank
attachment and common MCR/control mapping. Its model boundary is synthetic and
typed; its final dispatcher/allocation is synthetic. These scaffolds isolate
header authorization and emission. They do not test complete target timing or
platform composition. Existing LBP cases retain their production aggregation
and control paths. The cases cover four FRFCFS wrapper/capacity profiles and
58 atomic rejection cases across both profiles, including duplicate emission.

Compare all four immutable selected headers with:

```bash
/scratch/jfx/goldengate-circt-f2-build/goldengate-fased-header-test \
  --selected-goldens \
  /scratch/jfx/f2-sfc-goldens/rocket/attempt-20261021T132906Z-02537ef5/sfc/FireSim-generated.const.h \
  /scratch/jfx/f2-sfc-goldens/megaboomv3/attempt-20261021T133007Z-c3ba73a8/sfc/FireSim-generated.const.h \
  /scratch/jfx/f2-sfc-goldens/shuttle/attempt-20261021T133933Z-f1e312ec/sfc/FireSim-generated.const.h \
  /scratch/jfx/f2-sfc-goldens/saturn_shuttle/attempt-20261021T134041Z-08d813d4/sfc/FireSim-generated.const.h
```

The comparison extracts only the FASED constructor. It preserves ordered read
and write vectors, exact names/addresses, widget index, filename and memory-size
exponent. It normalizes raw C++ strings versus `std::string`, decimal `U`
suffixes, whitespace and backslash line continuations. Other headers and their
constructor order are outside this component comparison. The fixture sets
widget zero, base 128/size 128 for the distinct Rocket LBP recipe and
base zero/size 512 for each exact DDR3 recipe, matching the archived SFC
constructors. The positive unit cases separately test widget two/base 2048 and
its corresponding addresses. The earlier three-DDR3-only option
`--frfcfs-goldens MegaBoom.const.h Shuttle.const.h Saturn.const.h` remains
available. Rocket uses the real LBP aggregation and common MCR control fixture;
it is not substituted into the DDR3 comparison.

The main agent's centralized build and execution pass. The focused header
CTest passes, including the four positive FRFCFS profiles and 58 atomic
rejections; all three exact normalized golden ABI comparisons pass. Evidence:

- `/scratch/jfx/goldengate-circt-f2-build/iteration8-rebuild.log`
- `/scratch/jfx/goldengate-circt-f2-build/iteration8-ctest.log`
- `/scratch/jfx/goldengate-circt-f2-build/iteration8-header-goldens.log`

The initial build log preserves a test-only `NamedAttrList` API compile error;
the corrected test uses `get` plus `cast`, and the successful rebuild supersedes
that failure.

The final four-target invocation also passes with exit zero: Rocket's exact LBP
constructor and all three exact DDR3 constructors match the same normalization
policy. Final build and execution evidence:

- `/scratch/jfx/goldengate-circt-f2-build/iteration8-header-final-build.log`
- `/scratch/jfx/goldengate-circt-f2-build/iteration8-header-four-goldens.log`

Neither this implementation nor this finite header comparison establishes the
four-target semantic, Jasper LEC/SEC, F2 runtime or compiler-speed acceptance
nodes. The immutable SFC files and Scala oracle are read only.
