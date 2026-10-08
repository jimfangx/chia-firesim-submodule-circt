# Metasim interface headers

`MetasimInterfaceHeaderTest` checks both `.const.h` and `.const.vh` output
annotations against the validated native FPGATop/F1Shim ports. It exercises
non-default AXI widths, presence macros, safe Verilog include guards, retained
annotation/body preservation, and atomic rejection of either duplicate header,
inconsistent interfaces, and missing metadata. The optional boundary invocation
expects a snapshot before header preparation and an existing output directory.

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
