# Metasim interface headers

`MetasimInterfaceHeaderTest` checks both `.const.h` and `.const.vh` output
annotations against the validated native FPGATop/F1Shim ports. It exercises
non-default AXI widths, presence macros, safe Verilog include guards, retained
annotation/body preservation, and atomic rejection of either duplicate header,
inconsistent interfaces, and missing metadata. The optional boundary invocation
expects a snapshot before header preparation and an existing output directory.
That invocation also verifies the complete production IR before and after
preparation and emits `metasim-all.json` for comparison with the CLI archive.

The incoming-NIC production path emits `post-fame-metasim-header-all.json`
and the interface fragments under `metasim-interface/`. These fragments are
partial compiler outputs; they do not contain the NIC bridge constructors.
The preceding shim boundary remains available before header preparation.

The subsequent transactional composer emits the complete NIC catalog under
`driver-header/FireSim-generated.const.h` and archives
`post-fame-driver-header-all.json`. It starts with the unextended interface
header and validates every widget on a private CIRCT clone. SimpleNIC requires
matching retained bridge identity and native hardware, and its constructor is
prepared after CPU stream allocation validation. No header is committed on an
early, NIC, or final FASED failure. The NIC path then restores the unextended
interface archive and enters the common host-clock, Xilinx specialization,
hierarchy normalization, XDC and native CIRCT RTL lowering pipeline. The common
header preparation publishes the complete catalog beside the final RTL.

`NICFinalRTLCompare.py --candidate <candidate-dir> --golden <SFC-design-dir>
--output <comparison.json>` checks the fully emitted design. It compares both
typed platform interfaces, all shim routes, all twelve driver constructors,
fixed-layout checks, target configuration, interface constants and DDR/XDC
collateral. Each generated-clock destination must also resolve through live
emitted instances to a Xilinx BUFGCE. The single documented target hierarchy
rename is normalized for the XDC comparison. These integration checks do not
establish NIC runtime or native SRAM model equivalence.

`NICDriverHeaderCompare.py` compares all twelve constructors, registration
phases, fixed-layout checks and target configuration with the completed NIC
SFC header. It independently replays the composer through
`goldengate-print-binding-test --rocket-native-driver-headers`, exercises
twelve atomic rejections, verifies CIRCT IR and annotation/module preservation,
and compiles both complete headers against the actual registry and driver
declarations. Invoke with `--candidate`, `--sfc` (the archived NIC `.const.h`),
`--native-tool`, `--cxx`, and a fresh `--output-dir`. This collateral comparison
does not establish NIC runtime or final RTL equivalence.

`NICMetasimInterfaceCompare.py` checks the production annotation prefix and
the independently re-emitted native archive/collateral, compares the complete
typed configuration against the archived NIC SFC header, and checks 31 width/direction
relationships against each SFC RTL interface. It compares all 12 Verilog
constants with the NIC golden (excluding Scala's malformed unused
`MEM_HAS_CHANNEL-1`) and compiles both C++ interface configurations
against the actual `core/config.h` driver types with static assertions.
The immutable U250 build-tree fixture supplies RTL only; it has no C++ header.
Invoke it with `--candidate`, `--sfc`, `--immutable-sfc`, `--native-tool`,
`--cxx` and a fresh `--output-dir`.

The immutable U250 reference is:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Neither recorded immutable fixture contains `.const.vh`. Compare the emitted
header's CTRL, MEM, and CPU_MANAGED_AXI4 ID/address/data constants against
FPGATop's AW/AR/B/R IDs, AW/AR addresses, W/R data and W strobes. Compare
QSFP_DATA_BITS against F1Shim's two physical TX/RX payload ports. These are 31
width/direction checks. Presence macros describe one memory channel, a CPU
managed endpoint, and zero target QSFP/FPGA managed endpoints. Physical QSFP
lanes supply the payload width without creating target channel presence macros.

For the recorded Rocket/U250 configuration the 12 constants are CTRL
`{12,25,32}`, MEM `{16,34,64}`, CPU_MANAGED_AXI4 `{16,64,512}`, CPU presence `1`,
QSFP width `256`, and memory channel 0 presence `1`. These also agree with the
retained SFC-generated header, except the native header omits SFC's unused
malformed `MEM_HAS_CHANNEL-1` macro (from the Scala inclusive channel loop).

`make/goldengate.mk` declares `.const.vh` alongside the other compiler outputs
for both compiler selections and copies the CIRCT-emitted file from
`circt-ingestion`. `make/verilator.mk` depends on that header before invoking
the driver submake. Checking the missing-header make graph and Verilator lint
does not replace harness-owned replacertl and metasim verification. The harness
must use a fresh output directory or archive the mutable legacy header first,
then require both the new CIRCT header and its copied top-level counterpart.

`NICCPUStreamHeaderCompare.py` checks the subsequent NIC CPU stream header
boundary. It compares the complete managed-stream constructor, including
incoming/outgoing vector order, names, independent DMA bases, count MMIO
addresses, queue capacities and beat widths, with the archived NIC SFC
`.const.h`. Both constructors compile against the actual FireSim
`CPUManagedStreamWidget` declaration. The native test also checks full MLIR
verification, module-body preservation, one-annotation-only mutation, and
malformed incoming allocation/payload/MMIO rejection. Invoke with
`--candidate`, `--sfc` (the header), `--native-tool`
(`goldengate-cpu-managed-stream-header-test`), `--cxx`, and a fresh
`--output-dir`. The CLI writes this partial collateral in `cpu-stream-header/`;
the SimpleNIC constructor follows at the next partial boundary. Complete NIC
RTL/runtime parity remains open.


`NICHeaderTest` tests the normal-mode `SimpleNICBridgeModule.genHeader`
constructor. The native emitter requires a validated CPU stream header and
checks unique live bank/control/transport/queue paths, paired read/write
bindings, nonoverlapping allocated MMIO, all six register names/permissions,
unreset write/sample/hold behavior, readback, write-only assertions, and MAC,
rate limiter and pause-field packing. It derives independent incoming/outgoing
stream indices from their shared transport catalogs and checks their live
3072-by-512 queue storage. Queue-local indices cannot determine driver indices.
Only the `.const.h` OutputFile body and `goldengate.nicHeader` marker change;
validation failures are atomic.

The focused CTest relocates MMIO to 4096, widget index to 3, outgoing stream
index to 1 and incoming index to 2. It rejects 82 malformed metadata, register,
queue, allocation, hierarchy and annotation cases. Its optional recorded-input
invocation is `goldengate-nic-header-test <post-fame-f1-shim.mlir> <existing-output-dir>`;
it verifies complete production FIRRTL, preserves every module body, and emits
an independently prepared header and `nic-all.json`.

`NICHeaderCompare.py --candidate <candidate-dir> --sfc <golden-const.h>
--native-tool <goldengate-nic-header-test> --cxx <c++-compiler>
--output-dir <fresh-directory>` compares the complete SimpleNIC header fragment
with SFC, normalizing whitespace and Scala static-assert line continuations.
It checks the ordered struct layout assertions, all six MMIO addresses and all
four stream constructor arguments, and compiles both fragments against the
actual `generators/firechip/bridgestubs/src/main/cc/bridges/simplenic.h` ABI.
It also compares the independently emitted native archive/header with the CLI
and proves only the intended annotation changed.

The CLI archives this boundary in `post-fame-nic-header-all.json` and emits
partial collateral under `nic-header/`. Complete NIC driver assembly and final
RTL integration now follow this partial boundary. The NIC constructor match does not
establish a working combined NIC driver or runtime equivalence.

TracerV now resolves its shared CPU stream index by catalog identity rather
than using the producer queue's local index. The NIC recipe assigns outgoing
index one to TracerV while that queue's local index remains zero. The emitter
checks unique catalog names/ports, ordinal indices, queue geometry, address
windows, the selected typed transport port and every native address grant.
It preserves the existing single-stream constructor and rejects malformed or
ambiguous allocations before changing the OutputFile annotation.

The focused TracerV CTest covers shared indices one and two, relocated widget
and MMIO allocations, rational clocks and 45 atomic rejection cases. Its
production mode is `goldengate-tracerv-header-test <post-fame-f1-shim.mlir>
<existing-output-dir> --nic`: it verifies input/output CIRCT IR, preserves
all module bodies and unrelated annotations, and rejects a duplicate call
without mutation. `TracerVHeaderCompare.py` takes the same five named
arguments as `NICHeaderCompare.py`, selecting this native tool instead.
It compares the complete TracerV fragment with SFC, including fifteen MMIO
words/layout assertions, stream index/depth, retirement width and rational
clock; both fragments compile against the actual TracerV driver declaration.

The NIC CLI emits this partial boundary under `tracerv-header/` and archives
`post-fame-tracerv-header-all.json`. Complete driver sections and final NIC
RTL integration now follow this partial boundary. A matching constructor does
not establish NIC RTL or runtime parity.
