# Normalized external ready/valid payloads — iteration 36

The native ready/valid wrapper now implements the external payload shape from
`SimUtils.buildChannelType` and `ChannelizedWrapperIO.payloadTypeMap`: exclude
the direct target-valid leaf, remove empty containers, recursively collapse
single-field containers, and strip root `bits_` prefixes. Zero-width integer
leaves participate in these shape decisions until ordinary type lowering.
The external host token carries `Valid(normalizedPayload)`; the retained target
keeps its original post-FAME type. CIRCT Subfield/Cat/Bits and signed casts bind
the two views through the existing queues. Wrapper activation retargets exact
annotation leaf names while preserving associated target clock identities.

The pass rejects ambiguous normalized names, already instantiated wrappers,
and unexpected port uses before changing types or building queues. TSI,
BlockDev, and FASED token mapping accept the normalized interface and resolve
its leaf field IDs by name. Their earlier flat contracts remain accepted.

Four production Scala probes pass: flat, nested, nested singleton with an
empty sibling, and a signed payload with a zero-width sibling. The native
compiler ingests each exact FIRRTL/annotation pair, applies the wrapper passes,
and verifies the resulting IR before and after activation. The exposed nonzero
leaf names, signed widths, and directions match Scala exactly: respectively
16, 18, 14, and 14 leaves across both orientations. The nested probe also
matches all 28 control bindings and 12 typed payload bindings. SFC removes
zero-width leaves during lowering; the native pre-lowering wrapper retains them.

Eight focused CTests pass: four hub clock/queued-pipe tests, ready/valid channels,
and the TSI, BlockDev, and FASED token engines. These include exhaustive signed
packing/unpacking, independent queue scoreboards, annotation target resolution,
and bridge handshake/payload mappings under both accepted interface shapes.
The first fresh Rocket compile exposed TSI's fixed field-order assumption;
the name-based descriptor check and normalized TSI mapping test cover that fix.
The subsequent full native Rocket compile emits simulator SystemVerilog and
annotated collateral with exit status zero and empty stderr.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration36-normalized-rv/`:

- `scala-{flat,nested,singleton,zero}/signed-wrapper.sfc.fir` and their exact inputs.
- `ingested-*-wrapper.mlir` and corresponding `.active.mlir` artifacts.
- `ctest.stdout`, `normalization-ctest.stdout`, `consumer-ctest.stdout`,
  `fased-ctest.stdout`, and `tsi-ctest.stdout`. The initial FASED test traversal
  failed on the new nested bundle; the corrected traversal passes.
- `boundary-comparison.json`, `comparison.stdout`, and `compare-boundaries.py`.
- `compiler-candidate/post-fame-ready-valid-wrapper.mlir`, simulator RTL,
  and `compiler.stdout`/`compiler.stderr`; attempt-one logs retain the TSI failure.

Against the immutable artifact
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
the fresh native wrapper matches 138 direct control bindings with no differences
and 70 payload bindings. Twenty current AXI bindings (user, region, W id, and
response) have no corresponding direct assignment in the recorded fixture.
This comparison establishes boundary correspondence, not width parity or
whole-design equivalence. Signed and nested behavior uses the fresh Scala probes.

Iteration 35's harness passed CIRCT replacertl and both required Verilator
regression suites. Gates for this change remain harness-owned; the UART-bearing
SFC baseline is still pending. Payloads must currently have a positive total
width and enumerate every data leaf. The next smallest porting step is Scala's
annotation-selected payload filtering, using a probe with an unselected sibling
to check normalization, queue width, and target identity.

# Nested ready/valid payloads — iteration 35

`FAMEReadyValidChannel.cpp` now traverses passive bundles recursively, resolves
annotation endpoints to leaf field IDs, and packs known-width UInt/SInt leaves
in declaration order. It excludes only the direct target-valid field. Nested
leaves named `valid` remain data. Zero-width leaves retain their target identity
without consuming queue bits. The pass preflights all pairs before rewriting;
missing, duplicate, and ancestor endpoints, nested flips, clocks, vectors, and
unknown widths fail without constructing queues.

The production Scala probe accepts a two-level bundle containing SInt<3>,
SInt<0>, UInt<4>, and a nested UInt<1> named `valid`, in both orientations. Its
annotation endpoints are deliberately reversed. Scala's `buildChannelType`
flattens the sole `group` container; the native wrapper retains input target
paths. After that explicit normalization, all 28 control bindings and 12 typed
payload bindings match the lowered Scala wrapper. SFC drops the zero-width
signed leaf during lowering; native retains it until normal type lowering.

Five focused CTests passed. The ready/valid CTest also passed after adding the
reordered-endpoint case. Native tests exhaust all 256 packed patterns in both
orientations and both endpoint orders, and retain the 60,000-cycle independent
queue scoreboards. Both flat and nested production Scala probes pass.

Evidence is under
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration35-nested-rv/`:

- `scala/post-fame.sfc.fir`, `scala/post-fame.sfc.json`, and `scala/signed-wrapper.sfc.fir`.
- `ingested-nested-wrapper.mlir`, `ctest.stdout`, `order-ctest.stdout`, and `native.stdout`.
- `boundary-comparison.json`, `comparison.stdout`, and `compare-boundaries.py`.
- `compiler-candidate/post-fame-ready-valid-wrapper.mlir` and emitted simulator RTL.

The fresh Rocket candidate matches 138 direct ready/valid control bindings and
70 payload field bindings in the immutable U250
`...sfc-golden-2026-10-01/design/FireSim-generated.sv` artifact whose full path
is recorded below. Twenty current AXI payload bindings still have no matching
direct assignment in that fixture. The recorded Rocket payloads are flat and
unsigned; this comparison does not prove nested behavior, width parity, or
whole-design equivalence. Nested behavior uses the executable Scala probe.

Iteration 34's harness passed CIRCT replacertl and the required Verilator
suites. Gates for this change remain harness-owned; the UART-bearing SFC
baseline remains pending. The next small step is matching Scala's singleton
payload/container normalization at the external wrapper interface, using this
nested probe to check names and types as well as leaf correspondence.

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
