# Signed ready/valid payloads — iteration 34

The production `SimUtils.buildChannelType` retains both UInt and SInt leaves.
`SimWrapper.genReadyValidChannel` connects those typed leaves through its
queues. The native wrapper previously accepted only UInt leaves. It now packs
known-width passive integer leaves with `firrtl.asUInt` before concatenation
and restores signed dequeue leaves with `firrtl.asSInt` after extraction.
Each cast preserves the leaf's width and bit pattern. Signed zero-width leaves
retain their identity until type lowering; they do not consume packed bits.
Target-valid remains UInt<1>. Nested aggregates and empty total payloads remain
outside this implementation.

`FAMEReadyValidChannelTest` checks both bridge orientations with a signed first
leaf, signed last leaf, both signed, and one nonempty signed leaf. It exhausts
the 3/5-bit payload patterns, including signed minima and -1, and preserves the
existing independent FIFO scoreboards over 60,000 randomized cycles. Malformed
handshakes, clocks in payloads, flipped fields, and nested aggregates fail.
Five focused CTests passed; after adding the single-leaf case, the ready/valid
CTest passed again.

`FAMEReadyValidPayloadOracle.scala` elaborates the actual Scala SimWrapper with
SInt<3>, SInt<0>, and UInt<5> leaves in both orientations. It writes the
post-FAME input, annotations, and lowered Scala wrapper. The native compiler
ingests those exact files; the native test can consume that imported MLIR and
apply the actual wrapper transforms. Both signed external payload types and
all 28 host/target handshake and clock/reset connections match Scala. The
native wrapper preserves the signed zero-width field; SFC removes it during
lowering. This probe is separate from the immutable fixtures.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration34-signed-rv/`:

- `scala/signed-wrapper.sfc.fir` and `ingested-signed-wrapper.mlir`.
- `ctest.stdout`, `scalar-ctest.stdout`, and `native.stdout`.
- `boundary-comparison.json` and `comparison.stdout`.
- `compiler-candidate/post-fame-ready-valid-wrapper.mlir`.

The fresh Rocket native compiler also emits simulator SystemVerilog and
collateral with empty stderr. Its ready/valid wrapper matches 138 direct
transaction/control bindings and 70 payload field bindings in the immutable
artifact:

`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.

Twenty current AXI payload bindings (user, region, W id, and response fields)
have no corresponding direct assignment in that recorded emitted RTL. Those
differences are recorded explicitly; the comparison does not establish payload
width parity or whole-design equivalence. The recorded Rocket RV payloads are
unsigned, so signed behavior is checked with the executable Scala probe.

Iteration 33's harness passed CIRCT replacertl, the Verilator smoke test, and
both required Rocket suites. The UART-bearing SFC differential baseline remains
pending. The harness owns the gates for this change.

The next small semantic step is recursive packing of nested passive integer
payload leaves, using another production Scala wrapper probe. Shared producer
selection across transport kinds remains a model-channelization milestone:
Scala's `AddRemainingFanoutAnnotations` forks only PipeChannels, and
`SimWrapper.genPipeChannel` explicitly rejects aggregated pipe endpoints.
Mixed transport wrapper fanout therefore needs a separate supported contract.
