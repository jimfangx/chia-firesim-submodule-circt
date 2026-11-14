# Native F2 platform boundary

`BaseF2Config` selects the Scala `midas.EC2F2Config` interface. The native
compiler carries its 32-bit control address through every default widget,
decoder, dispatch, master and shell adapter. U250 retains 25-bit addresses.
Mixed or unsupported widths fail before changing IR.

Both profiles use one 16 GiB host-memory channel (34-bit address, 64-bit data,
16-bit ID) and a CPU-managed interface (64-bit address, 512-bit data, 16-bit ID).
F2 exposes no physical QSFP ports. Its physical PCIM interface uses 64-bit
addresses, 512-bit data and 6-bit IDs. With no peer streams, Scala leaves its
AW/AR/W payloads DontCare, drives their valid signals low and holds R/B ready
low. The native shim preserves that contract. The two control request ID
counters retain their accepted-request advancement and synchronous reset.
F2 XDC uses `firesim_top` before linking and `WRAPPER/CL/firesim_top` afterward.
The no-peer-stream driver interface still reports no FPGA-managed endpoint.

Oracle sources are `midas/Config.scala`, `configs/CompilerConfigs.scala`,
`midas/platform/F1Shim.scala`, and `midas/core/FPGATop.scala`. No Scala source
or immutable SFC output is changed by this port.

Focused validation (from a configured native build directory):

```sh
cmake --build . --parallel "${BUILD_JOBS:-1}" --target goldengate-circt \
  goldengate-f1-shim-test goldengate-metasim-interface-header-test \
  goldengate-fpga-top-shell-test goldengate-control-master-test \
  goldengate-simulation-master-control-test goldengate-control-write-dispatch-test \
  goldengate-control-widget-writes-test goldengate-control-read-dispatch-test
ctest --output-on-failure -R '^goldengate-(f2-compiler-config|f1-shim|metasim-interface-header|fpga-top-shell|control-master|simulation-master-control|control-write-dispatch|control-widget-writes|control-read-dispatch)$'
```

Set `BUILD_JOBS` from the available affinity/RAM budget. This session exposes
16 CPUs; an inherited `OMP_NUM_THREADS=1` makes `nproc` report one, so inspect
`os.sched_getaffinity(0)` and the actual cgroup limits as well. The control tests cover 32-bit
payload forwarding and regions ending at `2^32`, alongside U250 cases.
`goldengate-f1-shim-test --f2 <new-directory>/f2.mlir` emits a standalone shell
fixture with a passive typed FPGA-top stand-in for FIRRTL/HDL syntax checking.
That fixture is a focused platform test, not an acceptance simulator.

The compiler supports the following DDR3 fragments with either `BaseF2Config`
or `DefaultF2Config`. All use four ranks, eight banks, 16 GiB DRAM, and an
eight-entry transaction FIFO. Functional capacity is capped by constructor
`maxFlight`. Target data width, read/write burst lengths, and per-ID reuse are
derived from the bridge annotations. Power-of-two data widths from 8 to 1024
bits are supported with the existing 35-bit address/four-bit ID interface.

The Chia constructor `(dataBits, maxReadTransfer, maxWriteTransfer, idReuse,
maxFlight) = (256, 2, 2, 15, 232)` uses 32 active requests with FR-FCFS (16
with FCFS), a four-beat write ingress queue, four-bit acknowledgment counters,
and 512 read-buffer entries with 257-bit data/last packing. Read per-ID storage
rounds up to a power of two, matching Scala's MultiQueue. Driver headers and
memory-origin sidecars retain the derived widths.

A host-clock AXI adapter splits wider target beats or places narrower target
beats into F2's 64-bit memory port. It preserves IDs, byte strobes, response
errors, last-beat boundaries, and same-ID ordering under backpressure. Read
metadata and assembly state are maintained per ID. Converted bursts must fit
AXI's 256-beat limit. Constructors for which Scala selects read-ID translation
still reject explicitly; the Chia constructor does not require translation.

| Memory fragment | Base reads/writes | Command scheduler | LLC | MMIO words |
| --- | --- | --- | --- | --- |
| `FCFS16GBQuadRank` | 16/16 | One pipelined FIFO head | None | 58 |
| `FRFCFS16GBQuadRank` | 32/32 | Eight-entry first-ready window | None | 60 |
| `FRFCFS16GBQuadRankLLC4MB` | 32/32 | Eight-entry first-ready window | 4096 sets, eight ways | 68 |

The no-LLC paths connect target requests directly to the DDR3 controller and
controller completions directly to the AXI releaser. Their MMIO catalogs omit
all LLC words. FCFS also omits the first-ready window and transaction-depth
registers, which are declared only by Scala's `FirstReadyFCFSMMRegIO`.
The generated driver maps use 256-byte regions for the no-LLC profiles and
512 bytes for the LLC profile. Duplicate or conflicting memory fragments fail.

`DefaultF2Config` follows `midasexamples.BaseMidasExamplesConfig`: assertion and
print synthesis, multicycle RAM models, model multithreading, AutoILA,
AutoCounter CSV, and host token irrevocability assertions. It uses `F2Config`
directly, so its physical shell omits PCIM. `BaseF2Config` retains the EC2F2
PCIM interface described above.

`goldengate-f2-compiler-config`, `goldengate-fased-header`, and
`goldengate-fased-frfcfs-pipeline` cover configuration handoff, register maps,
functional capacities, and native composition for all three memory profiles.
`goldengate-fased-dram-profiles-rtl` lowers and lints both no-LLC pipelines and
also lowers the Chia geometry for all three memory profiles. It simulates
FIFO ordering, pipe replacement, refresh, open/closed page policy,
backend backpressure, and target-fire stalls with hardware assertions enabled.
`goldengate-fased-host-width-adapter-rtl` compares converted payloads,
IDs, strobes and error responses at 8, 32, 128, 256 and 512 bits, with fifteen
queued requests per ID and interleaved host responses. These component checks
do not compile the user's complete MegaBoom target.

These focused tests do not establish any of the four matched-input semantic
comparisons, Jasper LEC/SEC, F2 bitstream/runtime results or compiler speed
goals. CHIA retains ownership of those verification nodes. Their adapters stay
unset until real paired inputs and complete evidence are available.

Changed implementation files are `tools/goldengate.cpp`,
`include/goldengate/{ControlPortWidth,F1Shim,FPGATopShell,MetasimInterfaceHeader}.h`,
`lib/Support/MetasimInterfaceHeader.cpp`, and
`lib/Transforms/{ControlReadDispatch,ControlWidgetWrites,ControlWriteDispatch,F1Shim,FPGATopShell,SimulationMasterControl}.cpp`.
Focused tests change `CMakeLists.txt`,
`test/Support/{F2CompilerConfigTest.cmake,MetasimInterfaceHeaderTest.cpp}` and
`test/Transforms/{ControlMaster,ControlReadDispatch,ControlWidgetWrites,ControlWriteDispatch,F1Shim,SimulationMasterControl}Test.cpp`.
This document is new.

The 2026-10-21 focused build uses installed CIRCT `firtool-1.75.0`, GCC 15.3,
and Verilator 5.022. Build artifacts and logs are under
`/scratch/jfx/goldengate-circt-f2-build`; the complete compiler build used ten
parallel jobs after checking affinity and RAM. These are development checks,
with no compiler-time benchmark claim. The standalone shell commands are:

```sh
goldengate-f1-shim-test --f2 /scratch/jfx/goldengate-circt-f2-build/f2-shell-syntax/f2.mlir
firtool /scratch/jfx/goldengate-circt-f2-build/f2-shell-syntax/f2.mlir --format=mlir --verilog -o /scratch/jfx/goldengate-circt-f2-build/f2-shell-syntax/f2.sv
verilator --lint-only --top-module F1Shim /scratch/jfx/goldengate-circt-f2-build/f2-shell-syntax/f2.sv
```

No SFC golden comparison was run for this change. No four-target F2 golden
was created, overwritten or substituted with a historical U250 artifact.

Validation result: the full native compiler builds, all eight platform/control
CTests pass (452.25 seconds total), and the additional
`goldengate-f2-compiler-config` CTest passes. The final shim fixture also passes
`firtool` lowering and Verilator lint with no diagnostics. The eight-test log
is `focused-ctest.log`; configuration checks are in `f2-config-test.log`;
standalone fixture logs are in `f2-shell-syntax/`, all beneath the build path
above. `git diff --check` passes and the Scala oracle has no diff.
