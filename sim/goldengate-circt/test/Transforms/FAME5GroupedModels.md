## Production grouping update (2026-11-02)

The current full quad-Rocket input now compiles through model sharing and the
complete production F2 platform with `--compile-baseline --platform-config
MTModels_BaseF2Config`. Iteration 42 evidence is retained under
`/scratch/jfx/goldengate-circt-f2-build/iteration42-feature-implementation/fame5/`.
The unchanged-SFC dynamic RTL replay and native reset stress are supporting
checks; driver/workload and feature execution receipts are recorded separately.
The sections below the production update retain earlier bounded evidence and
limitations at the time of those checks.

The current implementation supports multiple independent selected groups. Each
has its own thread count, reachable banked definitions, host-cycle scheduler,
channel mux/demux and allocation record. A descendant shared by two original
groups receives a separate clone per group, preventing state or thread-count
aliasing. This extends the unchanged SFC boundary, which explicitly asserts
at most one group; it does not claim an unavailable multi-group SFC comparison.

Canonical Rocket `plusarg_reader` external sources are retained inside threaded
clones with their unchanged definition and WIDTH/FORMAT/DEFAULT parameters.
The supported ABI is exactly one positive-width UInt output `out`, no inputs,
and those three concrete parameters with WIDTH equal to the output width.
The primitive reads one simulator plusarg during initialization and emits its
DEFAULT in synthesis, so the same configuration value applies to every logical
thread. Timeout registers and comparisons remain banked and enabled normally.
Unknown external definitions and malformed readers reject before commit.

Known local whole-declaration DontTouch and DecodeTable metadata is copied to
the corresponding threaded declaration after checking it still resolves. A
DontTouch on a register protects the complete physical bank. Original metadata
is retained on the original definition. Unsupported metadata paths and unknown
annotation schemas still reject; consumed dedup and inactive optional pre-FAME
memory-model selections retain their source-definition provenance.

Abstract mode preserves the exact unchanged SFC local unreset-selector behavior
and its documented startup-phase relation. Production Xilinx mode now resets
every module-local selector with hostReset to the same zero slot as its top
arbiter. This establishes the previously assumed phase relation after host
reset and preserves it as all selectors advance on the same host edge. It
also resets the non-power-of-two accessor base counters to zero on hostReset.
Canonical constant-initialized hostClock/hostReset controls are reset across all
logical bank slots with distinct reset writers while disabling their normal
writer. This preserves the defined reset state of FAME enabled/fired flags even
though the selector holds slot zero during reset. Resetting only the selector
failed the actual vendor BUFGCE ledger because other flag slots stayed unknown;
that retained failed diagnostic motivated the bank-reset implementation.
Target register resets remain token controlled, and target/response RAM is never
cleared. Comparisons with unchanged
SFC remain scoped to its aligned-selector startup relation; arbitrary unreset
SFC startup is not a required defined payload match.

`goldengate-fame5-grouped-models-test` checks independent groups, allocation
records, production local reset ownership and positive/negative external source
cases alongside its existing exhaustive 2/3/4-thread routing and recursive
state/memory checks. Historical component evidence below retains its original
scope and assumptions; it is not current production Linux or formal acceptance.

# FAME5 repeated-model grouping and static channel scheduling

`multiThreadFAME5Models` performs real hardware sharing on an already FAME-
transformed circuit. It selects definitions from retained
`FirrtlEnableModelMultiThreadingAnnotation` instance targets, collects every
matching direct top-level instance in declaration order and skips definitions
with only one instance, matching unchanged `MultiThreadFAME5Models`.

For one repeated leaf definition, the pass creates a cloned threaded module,
banks its scalar register/reset state and supported zero/one-cycle-read target
memories, removes the repeated instances and creates one shared instance. A
resettable top-level host-clock counter routes each thread's input valid/bits
and output ready; input ready and output valid are masked by that thread's
selection. Output bits broadcast to all sinks, exactly as SFC `StaticArbiter`.
The selector advances every host cycle regardless of tokens or backpressure.
Grouping therefore reduces the actual number of instantiated datapaths and
constructs state and scheduling logic; it does not merely label annotations.

The model-local selector follows SFC `MuxingMultiThreader`: it is a free-running
host-clock register without reset. The top selector resets to zero on hostReset.
The supporting RTL checks start with a zero local phase (Verilator's initial
state) and hold reset for a complete rotation, starting at phase zero, so every
state bank is initialized and both selectors align. They repeat this aligned
reset midway through execution, and exercise independent per-thread target
clear, valid, enable and downstream stalls. These explicit initialization/phase
assumptions are not a formal proof about arbitrary uninitialized hardware state.

The whole transformation is staged in a cloned circuit and verified before
commit. It requires canonical whole-bundle connections between each model
channel and one distinct top port; hostClock/hostReset must be shared directly.
The CIRCT importer expands a flipped channel bulk-connect into its three
ready/valid/bits connects. The pass validates and reconstructs that exact
topology too, preserving the whole passive payload and requiring all fields to
connect to the corresponding fields of the same top port exactly once.
Unknown-width or active payloads, scalar non-host ports, noncanonical/fanout
connections, multiple groups, hierarchical target models, attached annotations/symbols
and retained debug targets needing renames reject without changing the original
circuit or result count. Existing state API restrictions still apply, including
scalar UInt synchronous memories and power-of-two memory-threading counts. Original module definitions and SFC's
consumed candidate selectors remain as provenance; SFC itself has an explicit
`TODO: Renames` at this boundary. Those selectors do not identify a live
post-sharing instance. Other model/removed-instance targets are rejected.

`FAME5GroupedModels.fir` deliberately declares repeated instances in order
`worker1, worker0, worker3, worker2`. The unit test covers two, three and four
threads, actual hardware sharing and memory depth, unchanged original
module/selector metadata, exhaustive ready/valid/bits routing, host-cycle
counter wrap, atomic rejections and the single-instance no-op. The Scala oracle
runs unchanged complete `MultiThreadFAME5Models.execute`, including unchanged
`MuxingMultiThreader` and `ImplementThreadedMems`, on the exact saved native
input/annotation files. Independent emitted-RTL scoreboards check two and four
threads for 8,192 cycles each, against per-thread replicated accumulator state.

The evidence is under `/scratch/jfx/fame5-grouped-models-20261024/`; its final
receipt records commands, hashes, assumptions and actual results. Both RTL
checks pass: two threads produce 16,384 channel observations and checksum
`129382100592920784`; four threads produce 32,768 observations and checksum
`64021397117480295`, identical between independently emitted SFC and CIRCT RTL.
The full retained selector annotation JSON also matches. Native unit cases
add 64/576/4,096 exhaustive observations for two/three/four threads respectively,
six atomic rejections and a single-instance no-op.

Native reproduction:

```sh
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-fame5-grouped-models$' --output-on-failure
/scratch/jfx/goldengate-circt-f2-build/goldengate-circt \
  sims/firesim/sim/goldengate-circt/test/Transforms/FAME5GroupedModels.fir \
  --annotation-file sims/firesim/sim/goldengate-circt/test/Transforms/FAME5GroupedModels.anno.json \
  --output-dir /scratch/jfx/fame5-grouped-models-20261024/native4 \
  --multi-thread-fame5-models
```

Compile `FAME5GroupedModelsOracle.scala` with Scala 2.13.10 and the existing
MIDAS classes/dependency classpath, then invoke
`midas.passes.fame.FAME5GroupedModelsOracle input.fir input.anno.json output-directory`.
Both emitted RTL arms use `FAME5GroupedModelsRTL.cpp` with Verilator
`--cc --exe --build --top-module Top -CFLAGS -DTHREADS=4` (or `2`).

These component checks do not establish the required enabled Linux feature case,
separate Jasper LEC/SEC or F2 execution. Full FAME5 still requires hierarchical
threading, retained annotation renames, dynamically gated target-clock staging,
complete production-pipeline enablement and the feature acceptance receipts.

## Grouped synchronous memories and clock infrastructure

`FAME5GroupedSynchronousMemory.fir` now exercises the complete grouped path with
one synchronous depth-three UInt memory, reset-defined per-thread register state,
static channel mux/demux and the unchanged SFC `AbstractClockGate` external ABI.
The gate is retained once inside the shared worker, not cloned or threaded as a
target child model. The supported bounded gate form has input `I` resolving to
`hostClock` and `CE` resolving to `UInt<1>(1)` through unique acyclic aliases.
Other children, gate symbols/annotations/parameters, invalid or multiple gate
input drivers and dynamic enables reject atomically. Target memory response
banks use the same internal free-running selector as the register state.

The existing grouped unit includes two/four-thread memory integration and five
atomic clock-infrastructure rejection cases. The unchanged complete Scala
`MultiThreadFAME5Models.execute` remains the semantic oracle; its own
`DefineAbstractClockGate.impl` supplies the identical gate implementation to
both independent RTL arms. Retained selector annotations match exactly. SFC's
additional `InlineAnnotation` names its generated `ThreadedMem` wrapper; native
constructs target/response banks directly inside the shared worker. The receipt
records raw annotations and normalizes only this checked helper-lowering directive.
The supporting scoreboard checks valid/ready, state,
same-address old-data collisions, mask suppression and independently stalled
transactions for 16,384 cycles per thread count. It performs no RAM clearing or
initialization sweep: ordinary successful masked writes establish the defined
word set. A read is compared only when enabled on a prior transaction and its
word was already written before that edge; unwritten/disabled results are
excluded from payload checks and checksums. Reset-defined counter state and
valid/ready remain strict. Mid-run aligned resets preserve written memory.
Different undefined response samples must pass with identical checksums;
corrupting a defined written response, using a wrong mask interpretation or
changing ready must fail. These focused checker controls do not replace the
formal initial-memory representation relation required for LEC/SEC.

Evidence and exact commands are under
`/scratch/jfx/fame5-grouped-sync-20261025/`. The receipt distinguishes this
supporting case from full production FAME5 acceptance. This memory fixture uses
constant clock-gate enables;
independent transaction stalls in this fixture use memory enables and channel
backpressure while the clock gate stays enabled. A direct-input toy worker with
a changing clock-gate enable is not a valid substitute for that staging. No
alternate gate model is used to accept it.


## Dynamic virtual-clock FAME controllers

`FAME5DynamicClockOracle.scala` runs the complete unchanged `FAMETransform`
on a target accumulator and extracts the emitted virtual-clock Worker unchanged.
A canonical two/four-instance wrapper gives both grouping implementations the
same post-FAME FIRRTL and selector annotation. The complete unchanged
`MultiThreadFAME5Models` oracle then emits the comparison RTL; the shared model
retains the exact unchanged SFC `DefineAbstractClockGate.impl` simulation model.
The checked-in `FAME5DynamicClock.fir` is this generated four-thread input, not
an immutable SFC golden or a hand-written replacement for FAME control logic.

The diagnostic virtual-controller classifier recognizes a deliberately narrow shape: the exact three-port
`AbstractClockGate` ABI driven by `hostClock`, and CE equal to
`(enabled & finishing) & ~hostReset`. The enabled flag is a synchronous host
register reset to zero, updated by `mux(finishing, 1, enabled)`. After register
threading, recognition validates the corresponding host-owned async-read bank,
same read/write slot, constant read/write enables and mask, and the reset/data
mux. A register-bank marker alone never authorizes a mutated clock controller.
Direct dynamic channel CE, omitted reset mask, different flag reset/update,
target-clock-owned enable flags and duplicate flag drivers reject atomically.
This classifies a virtual-clock FAME controller; arbitrary dynamic gates,
clock-token hub controllers and multiple generated target domains do not classify.
Production grouping still rejects this dynamic shape atomically with an explicit
target-edge/local-thread phase diagnostic. Classification is not permission to
thread a dynamically clocked model.

`FAME5DynamicClockRTL.cpp` checks the complete emitted model over 16,384 host
cycles for each two/four-thread case, including independent valid/ready stalls,
ordinary target reset tokens, and an aligned whole-rotation host reset. Its
independent per-thread input/output fired-flag model checks ready/valid strictly.
Target-state observations begin after ordinary target reset transactions; no
unwritten RAM or arbitrary initial state samples are used. Payloads are compared
against the actual unchanged complete SFC RTL, with invalid output payloads
excluded. No target RAM is cleared or initialized. The fixture starts the
unreset local selector and gate latch at Verilator's zero phase; this is a
supporting simulation assumption, not a reset initialization proof.

The independent per-thread target accumulator ledger produces a counterexample
when clock CE transitions 1 to 0 to 1: in the two-thread SFC output, cycles
68--70 disagree with an atomic selected-thread target edge. The exact gate
simulation model samples CE with a nonblocking posedge assignment, while the
local selector also advances on that host edge. The preservation of this phase
relationship has not been established. The counterexample is retained; changing
the gate model or accepting SFC/native trace equality would not discharge it.

Evidence lives under `/scratch/jfx/fame5-dynamic-clock-20261025/` and records
exact commands, inputs, gate implementation, compiler/oracle hashes, retained
annotations and actual CLI rejection checks. This component is not
full FAME5 acceptance: production pipeline composition and a Linux-capable
threaded representative still require separate semantic, LEC, SEC and F2 Linux
feature receipts. Dynamic synchronous RAM response qualification, complete target token/phase
correspondence and functional dynamic gate enablement remain explicit integration
obligations. This iteration does not claim dynamic FAME5 compiler parity.

## Explicit Xilinx dynamic-clock specialization (recovery)

The abstract-gate diagnosis above is retained. `FAME5ClockSemantics::XilinxBUFGCE`
and the diagnostic `--multi-thread-fame5-models-xilinx` entry point now group
canonical dynamic virtual controllers before applying the unchanged SFC Xilinx
host specialization. This choice matches the production platform sequence:
`BUFGCE` defaults to a synchronous low-phase CE latch, whereas the abstract
simulation gate above samples CE at the rising edge. The compiler preserves the
primitive and its original CE/clock wiring; it does not replace it with a
combinational clock expression.

The supported dynamic state is scalar synchronous registers and scalar target
RAMs with zero-cycle reads and one-cycle writes. Memory addresses become
`{original address, local thread}`. Original masks, enables, data and the gated
write clock remain intact. Nonzero-latency reads still reject atomically because
the response/tag pipeline phase needs its own proof. Register and target RAM
initialization remain unchanged, and the SFC local free-running helper selector
still needs an explicit startup-phase correspondence with the top scheduler.

`FAME5DynamicMemoryOracle.scala` derives its virtual controller through the
complete unchanged SFC `FAMETransform` and then calls complete unchanged grouping
and Xilinx specialization. `FAME5DynamicMemory.fir` is the resulting four-thread
input fixture. It includes an asynchronous-read, masked-write target memory,
three banked host control registers, and an input-dependent output-valid cone.
`FAME5DynamicClockCompare.py` compares the actual SFC and native RTL with AMD's
installed `BUFGCE.v` and `glbl.v`, independent per-thread register/memory ledgers,
strict channel controls, arbitrary independent stalls, and an aligned host reset
in the middle of traffic. Input tokens remain stable until accepted. Only the
helper selector startup is set; target state and RAM are never cleared by the
testbench. The memory ledger checks payload only after that thread/address was
written with its mask set. The register ledger checks only target-reset-defined
state. Different unqualified payloads must pass with the same checksum, while
ready, valid and defined-payload corruption must fail.
An additional actual RTL fault control forces the target write mask high in a
separate compiled input and requires the same written-memory ledger to reject.
The memory comparison discharges only SFC's generated `Top.ThreadedMem`
`InlineAnnotation`: native banking directly places that storage in the shared
model and creates no wrapper needing inlining. All retained annotations match.

This is bounded compiler supporting evidence. It does not establish dynamic
synchronous memory response behavior, hierarchical grouping, a Linux-capable
threaded production representative, Jasper LEC/SEC, or F2 feature acceptance.


## Dynamic synchronous memory responses (iteration 37)

The previous recovery section records the earlier asynchronous-only scope.
Explicit Xilinx grouping now also accepts scalar one-cycle target reads when all
accessors use the same canonical virtual controller. The memory API requires an
explicit `FAME5MemoryClockSemantics::XilinxBUFGCE` choice to admit that controller;
its default still rejects dynamic accessor clocks. Malformed controllers,
unrelated generated clocks and mixed accessor clocks remain unsupported.

The target bank keeps its original gated clock, read/write enables, masks and
old-data collision semantics. The existing SFC response algorithm detects target
edges with a toggling target-clock register and its host-clock tracker, delays
the selected thread tag and banks each accessor's response under hostClock.
Two-thread response banks use asynchronous reads; four-thread response banks add
the SFC data/edge pipeline and synchronous response-bank read. No target memory,
response memory or helper register reset/initialization is inserted by the
compiler.

`FAME5DynamicSynchronousMemoryOracle.scala` derives the model through complete
unchanged SFC FAMETransform, grouping and Xilinx specialization.
`FAME5DynamicSynchronousMemoryCompare.py` runs the actual AMD BUFGCE/glbl model
and an independent per-thread written-memory ledger. It checks ready/valid
strictly and qualified response payloads after an enabled read of a previously
written exact thread/address, using pre-write data on read/write collisions.
Responses after disabled reads or stalled target cycles are unqualified. The
ledger never preclears target RAM, response RAM, read-data or tag pipelines.

The local unreset selector starts aligned with the top reset scheduler. All four
independent concrete Boolean initial values of edgeCount/edgeCountTracker are
checked, followed by a full thread rotation with hostReset asserted (target CE
masked). The later host reset also spans a whole rotation. This is explicit
bounded startup coverage, not an arbitrary-X initialization proof. Independent
input/output stalls, irrevocable input payloads, masked writes and collisions
are exercised; invalid/unwritten payload perturbations must pass, and written
payload/control corruption and an actual forced target-write-mask fault must
fail. A separate actual RTL fault complements the response-bank write-thread
tag and must fail the same ledger, detecting cross-thread response placement.
The sole structural annotation normalization discharges SFC's generated
ThreadedMem inline hint because native storage is directly banked.

This extension is supporting compiler evidence. Recursive model hierarchy,
general target clock domains, a production threaded Linux representative,
separate Jasper LEC AND SEC and required F2 feature receipts remain incomplete.


## Recursive internal model hierarchy (iteration 38)

Grouping now recursively visits the selected model's reachable internal module
definitions in postorder, matching unchanged SFC MuxingMultiThreader and
AddHostClockAndReset. It creates one banked clone per reachable definition and
retains each child instance, including siblings referencing the same definition.
Original definitions remain unchanged. Each clone has its own SFC free-running
host-clock selector; target register/memory clocks and target reset wiring remain
intact. Missing hostClock/hostReset inputs are appended and connected to the
parent host controls. Existing host inputs require their exact ABI and one
canonical parent host connection, rather than inventing clock/reset ownership.
Child instance names and name-preservation metadata remain intact.

All reachable modules, ports and instances are preflighted. Recursive module
cycles, unknown external modules, bind/layer child instances, noncanonical host
connections and annotation/symbol targets needing renames reject atomically.
Canonical clock-gate infrastructure retains the existing abstract/Xilinx mode
guards. Direct and relative descendant annotation paths into cloned definitions
also reject, including paths rooted at a removed selected top instance.

`FAME5Hierarchy.fir` contains Worker -> two Branch instances -> separate Leaf
instances. Every level has independent scalar state; each Leaf also owns masked,
asynchronous-read depth-three target memory. The input clock/reset ports on
Branch/Leaf remain target ports; the new host ports are distinct. Complete
unchanged SFC grouping emits the oracle through `FAME5HierarchyOracle.scala`.
`FAME5HierarchyCompare.py` checks two/four threads for 16,384 host cycles against
an independent per-thread, per-sibling state/memory ledger. Channel controls,
reset-defined state and previously written exact thread/sibling/address payloads
are strict. No target RAM initialization is inserted. Host reset spans one whole
rotation at startup and during traffic; input tokens remain stable until
accepted. Invalid/unwritten payload perturbations pass. Ready, valid and defined
state corruption fail, as do actual compiled write-mask and sibling-data wiring
faults. A leaf selector shifted by one startup slot also preserves the public
ledger under a consistent per-leaf bank permutation; the normal case starts all
selectors at zero. This bounded check does not prove arbitrary-X startup.

The sole structural annotation normalization discharges SFC's generated
ThreadedMem inline hint because native storage is placed directly in Leaf.
Production threaded Linux integration, general dynamic clock composition,
separate complete Jasper LEC AND SEC and actual F2 feature acceptance receipts
remain required. This compiler component evidence does not populate feature
acceptance receipts.


## Three-thread descendant memory mapping (iteration 39)

Recursive grouping also supports the unchanged SFC non-power-of-two memory
branch. A three-thread Leaf has two independent unreset host-clock accessor
base counters as well as its module-local selector. Its sibling instances retain
separate helper and target state. The grouped unit checks this hierarchy and the
existing two/four-thread cases; the complete SFC hierarchy comparison records
strict controls/reset-defined state and only previously written exact
thread/sibling/address payloads. Helper base counters start at zero alongside
local selectors in the bounded simulation. No counter or target RAM reset is
added. This is additional compiler geometry coverage; it does not establish the
required enabled Linux FAME5 representative or arbitrary-X startup equivalence.

`FAME5HierarchySynchronousMemory.fir` additionally applies the same recursive
case to one-cycle descendant reads, independently banking each sibling's target
memory, target-edge detector, thread tags and response memory. The independent
ledger uses the prior enabled read's pre-write data, preserving old-data
collisions and ignoring only unwritten or invalid responses. Four concrete
Boolean target-edge/tracker startup pairs are exercised without initializing
response memory or tag pipes. Aligned midtraffic host resets preserve the
written set. Arbitrary host-reset phase is a reference limitation described in
`FAME5ThreadedMemory.md`; no production alignment guarantee has been established.
