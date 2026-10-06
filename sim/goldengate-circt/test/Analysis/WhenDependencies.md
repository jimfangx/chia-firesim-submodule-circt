# FAME dependency normalization regression

`MidasTransforms.scala` lowers FIRRTL before `FAMETransform.scala` consumes
`CheckCombLoops` connectivity. Conditional assignments must therefore become
muxes, and repeated assignments must resolve by last-connect priority before
channel dependencies are computed. The normal CIRCT compiler calls
`normalizeFAMEInput` at the target-lowering boundary. Generic retained-target
type lowering remains usable on partially connected analysis fixtures.

`goldengate-comb-dependency` checks nested predicates, overwritten connections,
register boundaries, retained register identity, and explicit rejection of
unnormalized conditional/multiple drivers. It uses actual CIRCT FIRRTL
operations and passes.

The October 6, 2026 comparison extracts `Queue1_AXI4BundleW` verbatim from the
immutable compiler fixture:

```
sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

Lines 27307–27364 contain the module; lines 27351–27352 conditionally assign
`io.deq.valid` under `io.enq.valid`. A probe wrapper supplies a concrete UInt1
reset and connects the whole interface without changing the extracted module.
The compiler's `--lower-types` import produces the ground MLIR test input.

The corresponding immutable U250 artifact is:

```
sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

Its lines 58705–58706 show `io_enq_ready = ~maybe_full` and
`io_deq_valid = io_enq_valid | ~empty`. The CIRCT analysis must report exactly
`io_deq_valid <- {io_enq_valid}` and `io_enq_ready <- {}`; `empty` and
`maybe_full` follow registered state. The test also checks the probe's
hierarchical valid dependency. `io_count <- {}` is checked against the
FIRRTL register boundary; this unused port was eliminated from the SFC RTL.

The mutable extraction, before-normalization IR, normalized IR, and comparison
log are under the U250 generated-source directory's
`iteration1-when-dependencies/`. Recheck that input with:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
"$gg_generated/goldengate-circt-build/goldengate-comb-dependency-test" \
  "$gg_generated/iteration1-when-dependencies/before/post-lower-types.mlir" \
  "$gg_generated/iteration1-when-dependencies/normalized-queue.mlir"
```

This comparison establishes the queue's combinational dependency boundary.
Full transformed-Rocket compilation and behavioral gates remain harness-owned.

The October 6 iteration 2 comparison extends this same immutable queue to the
payload fields. FIRRTL lines 27347–27354 contain the constant-address read and
empty-queue bypass; U250 RTL lines 58698–58700 fix the read enable to one and
address to zero. RTL lines 58707–58709 select each enqueue field or its stored
RAM value. The candidate must report exactly:

```
io_deq_bits_data <- {io_enq_bits_data}
io_deq_bits_strb <- {io_enq_bits_strb}
io_deq_bits_last <- {io_enq_bits_last}
```

All three paths are checked both in the queue and through the probe instance.
There are no dependencies on the clock, write controls, or other payload fields.
The mutable artifacts are in `iteration2-memory-dependencies/` beside the
earlier extraction. `comparison.log` records the C++ analysis assertions;
`normalized-queue.mlir` contains the corresponding normalized candidate IR.

The related vector-selection regression exposed a gap in the reusable analysis:
selected vector memory data was rejected as `memory result` because the
immediate SSA operation was a `SubindexOp`. Memory reads now use the canonical
memory-port `FieldRef`, decode the selected top-level port field, and follow
address and enable by field ID. The C++ test checks static selections and node
aliases from a vector memory with ground model ports; asynchronous reads depend
on address/enable, while synchronous reads depend on neither. The failing
pre-fix test reported `unresolved output async: Model:memory result`.

The FireSim dependency is `firrtl_2.13:1.6.0`. Its
`firrtl/transforms/CheckCombLoops.scala` lines 144–150 add address and enable
edges for zero-latency reader ports. `FAMETransform.scala` consumes that
connectivity to form output-channel dependencies. Asynchronous readwrite ports
need a separate write-mode investigation and now produce an explicit blocker;
new read-under-write retains its existing blocker. Both rejection paths are
tested, so unsupported memory behavior cannot appear dependency-free.

The October 6 iteration 3 comparison covers variable-address register-file
reads and their explicit write bypass. The unchanged statements from the same
immutable compiler artifact's lines 145182–145198 and 146396–146408 are placed
in `RFReadProbe`. Its inputs expose `id_raddr1`, `id_raddr2`, `rf_wen`,
`rf_waddr`, and `rf_wdata`; its outputs expose both the initial memory reads
and the bypassed values. This is an extracted internal boundary, not a
replacement for compiling or simulating the whole Rocket module.

The immutable U250 `design/FireSim-generated.sv` lines 124018–124030 fix both
read enables to one, invert each corresponding read address, and read stored
RF state. Lines 123611–123618 apply the write-address comparison and write-enable
bypass. The imported and normalized candidate matches these dependency sets:

```
raw0  <- {id_raddr1}
raw1  <- {id_raddr2}
read0 <- {id_raddr1, rf_wen, rf_waddr, rf_wdata}
read1 <- {id_raddr2, rf_wen, rf_waddr, rf_wdata}
```

The comparison also repeats the earlier queue assertions. The mutable FIRRTL
extraction, imported MLIR, normalized MLIR, and comparison logs are in
`iteration3-field-dependencies/` under the same generated-source directory.
Run the boundary assertion with `goldengate-comb-dependency-test` using
`candidate/post-lower-types.mlir` and optionally `normalized-rf.mlir` there.

A related C++ regression packages two independently addressed reads and an
unrelated input into aggregate wires, aliases one wire with a node, and selects
fields from an aggregate mux. Before the fix it fails with
`unresolved output alias: Model:undriven wire`: tracing the immediate subfield
discarded the selected field ID before reaching its driver. Node and mux
tracing now forwards the canonical field ID. The chosen read follows only
the selector and its two address inputs, in Scala operand order. Selecting
the other field follows only the selector and the unrelated input, which
also checks that memoization keeps the two field identities separate.

The October 6 iteration 4 comparison covers TLError's dynamic opcode lookup.
The unchanged table and lookup statements from the immutable `.sfc.fir`
lines 18828–18837 are placed in `OpcodeProbe`. Ground ports expose the original
`a_q.io.deq.bits.opcode` index and `da.bits.opcode` result. The table has eight
entries with values `0, 0, 1, 1, 1, 2, 4, 4`. The same immutable U250
`design/FireSim-generated.sv` lines 51604–51609 and 51637 implement this table
as constant-valued muxes controlled only by `a_q_io_deq_bits_opcode`.

The candidate reports exactly `out <- {index}` both on imported FIRRTL MLIR
containing `SubaccessOp` and after the real `normalizeFAMEInput` pipeline
replaces it with `MultibitMuxOp`. This compares an extracted internal boundary,
not the whole TLError module. The mutable extraction, annotation placeholder,
imported and normalized MLIR, and comparison log are in
`iteration4-dynamic-dependencies/` under the same U250 generated-source tree.
Recheck it with:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
"$gg_generated/goldengate-circt-build/goldengate-comb-dependency-test" \
  "$gg_generated/iteration4-dynamic-dependencies/candidate/input.mlir" \
  "$gg_generated/iteration4-dynamic-dependencies/normalized-opcode.mlir"
```

The related C++ regression selects a bundle field through a dynamic vector
read and a node alias. Before the fix it fails with
`unresolved output chosen: Model:undriven wire`. The tracer now visits the
index and projects the selected relative field ID into each reachable vector
element. It excludes unrelated bundle fields, keeps separate cached field
identities, and stops at vector register state while retaining the read index.
Those dependency sets and their direct operand order match before and after
CIRCT normalization. Separate tests exclude elements unreachable by a narrow
index, select only the literal-index element, and reject an out-of-range literal.

SFC 1.6.0 `RemoveAccesses` emits guarded connections in ascending element order;
`ExpandWhens` gives later connections priority. The resulting mux expression
visits the selector and then higher elements first, also visible in the golden
RTL. This validates local expression order, not general graph-order parity:
SFC `CheckCombLoops` additionally simplifies connectivity using breadth-first
reachability, which remains a separate ordering question for deeper graphs.
At that checkpoint, whole-aggregate connects still required normalization to
resolve selected child-field drivers. The following comparison covers that gap.

The October 6 iteration 5 comparison extracts AXI4UserYanker's response echo
lookup. The same immutable compiler `.sfc.fir` lines 26572–26588 define
`_r_bits_WIRE` and drive each vector element with a whole-bundle connection.
Lines 26604–26605 dynamically select `tl_state.source` and `tl_state.size`.
`UserYankerProbe` retains those statements verbatim and exposes the ten queue
outputs for each field as ground inputs. It also retains the original invalid
assignments for entries 10–15 from lines 26779–26845. Those entries become zero
in the immutable U250 `design/FireSim-generated.sv` lines 57896–57901,
57920–57925, and 58321–58322; they are not additional boundary inputs.

RTL lines 57878–57925 construct separate source and size mux chains controlled
by `auto_out_r_bits_id`. Lines 58321–58322 assign their results to the response
echo fields. The imported and normalized candidate matches exactly:

```
source <- {index, source0, source1, ..., source9}
size   <- {index, size0, size1, ..., size9}
```

Neither output depends on the other field's queue inputs. The unchanged
extraction initially failed with
`unresolved output source: UserYankerProbe:undriven wire`. Driver indexing now
uses CIRCT `walkGroundTypes` to project each aggregate connection into leaf
`FieldRef` pairs, preserving the source field ID when tracing a selected sink.
Legal connect layouts have matching relative field IDs even when ground widths
differ. Flipped leaves reverse the pair, following SFC 1.6.0 `ExpandConnects`;
strict connects are passive. Overlapping aggregate/leaf assignments retain
the unresolved last-connect blocker until `normalizeFAMEInput` resolves them.

The C++ regression checks nested bundle/vector connections, passive strict
vector connections, mixed-direction ordinary bundle connections with differing
widths, isolated selected fields, and a leaf override before/after normalization.
Foreign connections receive an explicit unsupported-module blocker rather than
an invalid FIRRTL type cast. Three focused CTest cases passed:
`goldengate-comb-dependency`, `goldengate-lower-types-targets`, and
`goldengate-fame-output-selection`. Queue, RF, and opcode boundary comparisons
also passed again with the modified tracer.

Mutable artifacts are under the same generated-source tree's
`iteration5-aggregate-dependencies/`: `UserYankerProbe.fir`, `annotations.json`,
`candidate/input.mlir`, `normalized-user-yanker.mlir`, `before-test.log`,
`comparison.log`, and `tests.log`. Recheck the boundary with:

```sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh --skip-ssh-setup
gg_generated=sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config
"$gg_generated/goldengate-circt-build/goldengate-comb-dependency-test" \
  "$gg_generated/iteration5-aggregate-dependencies/candidate/input.mlir" \
  "$gg_generated/iteration5-aggregate-dependencies/normalized-user-yanker.mlir"
```

This establishes the internal lookup's field dependency boundary, not whole
UserYanker behavior or complete FAME port parity. General SFC breadth-first
dependency ordering remains the next small connectivity investigation; manager
compilation, Verilator metasimulation, and U250 gates remain harness-owned.
