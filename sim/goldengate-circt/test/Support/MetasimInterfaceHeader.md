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
and the interface fragments under `metasim-interface/`, then rejects the
remaining driver-header integration gap. These fragments are partial compiler
outputs; they do not contain the NIC bridge constructors. The preceding shim
boundary remains available before header preparation.

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
partial collateral under `nic-header/`. It still rejects complete NIC driver
assembly and final RTL integration. In particular, the existing TracerV driver
emitter assumes outgoing slot zero, whereas the NIC SFC fixture places TracerV
at slot one. The NIC constructor match does not establish a working combined
NIC driver or runtime equivalence.
