# Optional SRAM channel preparation

`--analyze-sram-channels` executes native FIRRTL/MLIR transformations through
`InferModelPorts`: SRAM wrapping, transitive model promotion, aggregate lowering,
passthrough promotion, default channels, clock inference, channel excision, and
module-local port grouping. This follows `MidasTransforms.scala`'s ordering and
stops at the graph consumed by `FAMETransform.scala`. The input must already have
a wrapped top, a target-clock channel, and resolved last-connect semantics.

The boundary emits `post-sram-channels.fir`,
`post-sram-channels-all.json`, and `post-sram-channel-dependencies.json`.
It binds global channels back to inferred local groups and runs the native
combinational analysis used by FAME output-valid generation. Unsupported paths
fail the boundary instead of producing an apparently independent output.
FIRRTL 1.2 statements and a large line margin let
the pinned Scala parser inspect the candidate directly. The comparator ignores
CIRCT's `public` module keyword, which the old parser does not understand.

## Oracle and comparison

The immutable compiler input used in iteration 45 is:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

`SRAMModelChannelsOracle.scala` uses unchanged Scala passes to prepare that
input in LowForm, select its real `Rocket` hierarchy and `rf` memory, add a
minimal 1:1 target-clock channel, and wrap the top. Both compilers consume the
same emitted `golden-rocket.channels-input.fir/json`. This isolates optional
SRAM behavior; the recorded configuration does not select SRAM models by
default. The script also prepares a multiport memory with four instances across
two hierarchy levels (`fanout.channels-input.fir/json`). Its wrapped input is
checked in as `SRAMModelChannels.fir/json` for CTest.

The comparator checks annotation multisets, top-port direction/type and actual
instance-port bindings, promoted model identities, and SRAM port widths,
memory depth, latency, reader/writer/readwriter names, and read-under-write
behavior. It maps legal hub instance-name differences using actual FIRRTL
connections. It compares both-ended pipe channels by endpoint identity rather
than their generated global name, preserving clocks, payload order and
multiplicity. Original top-level I/O names remain part of the ABI comparison.
The comparator follows direct, type-matched hub output aliases: SFC binds eight
Rocket top outputs to equivalent hub outputs instead of their explicitly
assigned aliases. Both artifacts contain those assignments. Only SFC's
compiler-history `DedupedResult` records are excluded.
No Scala hardware transformation runs on the native candidate.

The comparison exposed extra native annotations. `FAMEDefaults` now emits one
module label for repeated instances of that module, and `InferModelPorts`
protects a shared clock/port once. Existing labels/protection seed those sets.
These changes match SFC `runTransform`'s annotation uniqueness without removing
unrelated annotations. The fanout mismatch was three duplicate module labels
and 48 duplicate protection records; the Rocket mismatch was nine duplicate
protection records.

After the fixes, both comparisons pass:

| Input | Top models (including hub) | Top ports | Channels | Local groups | Annotation records |
| --- | ---: | ---: | ---: | ---: | ---: |
| Four-instance fanout | 5 | 115 | 53 | 66 | 197 |
| Golden Rocket register file | 2 | 537 | 11 | 21 | 100 |

The fanout also retains four distinct channel clock-source targets and 70
unique port-protection records. CTest checks the graph and pre-existing
annotation cases, plus valid emitted statement newlines. The four focused
tests pass; the Scala comparisons additionally check SRAM memory semantics.

## Reproduction

Source `sourceme-manager.sh --skip-ssh-setup` from `sims/firesim` first.
Compile both `ExtractSRAMModelsOracle.scala` and
`SRAMModelChannelsOracle.scala` with the pinned Scala 2.13 compiler and Midas
dependency classpath. Then, with `evidence` pointing to a mutable output
directory inside the repository:

```sh
java -Xmx12G -cp "$oracle_classes:$midas_classpath" SRAMModelChannelsOracle \
  "$evidence/oracle" "$immutable_sfc_fir"
for name in fanout golden-rocket; do
  "$native_compiler" "$evidence/oracle/$name.channels-input.fir" \
    --annotation-file "$evidence/oracle/$name.channels-input.json" \
    --output-dir "$evidence/$name-candidate" --analyze-sram-channels
done
java -Xmx4G -cp "$oracle_classes:$midas_classpath" SRAMModelChannelsCompare "$evidence"
ctest --test-dir "$native_build" --output-on-failure \
  -R 'goldengate-(sram-model-channels|extract-model|label-sram-models|fame-output-selection)$'
```

Iteration 45 artifacts are under the mutable U250 generated directory's
`iteration45-sram-channels/`: oracle inputs/results, native candidates,
`comparison.log`, `ctest.log`, and `baseline.stdout/stderr`. The fresh native
baseline also passed through all FAME channel/control rewrites and PrintBridge
host binding with empty stderr. Manager verification remains harness-owned.

## Dependency boundary (iteration 46)

The new analysis exposed a clock-binding mismatch. SFC projects a global
channel clock into `clockPort` only for the module exporting that clock.
The SRAM on the other end has no local `clockPort` and uses FAME's virtual
clock channel. Native binding now accepts that remote clock while continuing
to require exact port/instance identity on the owner. Unit tests reject a
missing owner clock, a falsely RAM-local clock, and a clock from the wrong side
without changing IR.

Native combinational analysis now emits one result per module-local output
group, including when four SRAM instances or multiple transport branches
bind the same group. Every global channel and binding remains present.
`test/Analysis/SRAMChannelDependenciesOracle.scala` compares these results
with the unchanged `FAMEChannelAnalysis`/`CheckCombLoops` graph using
`FAMETransform`'s LI-BDN step 2. It compares input-channel sets because AND
dependency order does not affect output validity, and rejects duplicate
native output identities. It also writes the SFC dependency artifacts.

Fresh preparation from the immutable `.sfc.fir` above produces:

| Input | Model definitions | Local outputs | Dependency edges |
| --- | ---: | ---: | ---: |
| Four-instance fanout | 2 | 46 | 0 |
| Golden Rocket register file | 2 | 10 | 4 |

Both graphs match SFC. The synchronous reader/readwriter outputs have no
combinational input dependencies. Each of Rocket's two asynchronous register
file reads depends on its own address and enable channels; write inputs do
not enter those output-valid conditions. The structure/annotation comparison
still passes with the counts above. All six focused tests pass, including
combinational dependency and channel-clock-domain regressions. A fresh native
baseline through PrintBridge host binding passes with empty stderr.

Artifacts are under `iteration46-sram-dependencies/` in the same mutable
U250 generated directory: `oracle/golden-rocket.channels.sfc.fir/json`,
`oracle/golden-rocket.dependencies.sfc.json`, the corresponding native
`golden-rocket-candidate/` outputs, fanout artifacts, build and comparison
logs, `ctest.log`, and `baseline.stdout/stderr`. Compile the additional
`test/Analysis/SRAMChannelDependenciesOracle.scala` alongside the two oracle
files above, then run:

```sh
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMChannelDependenciesOracle "$evidence"
ctest --test-dir "$native_build" --output-on-failure \
  -R 'goldengate-(sram-model-channels|comb-dependency|fame-output-selection|fame-channel-clock-domains|extract-model|label-sram-models)$'
```

## Virtual-clock hardware boundary (iteration 47)

`--rewrite-sram-clocks` runs the native preparation/dependency boundary above,
then constructs virtual-clock hardware on the SRAM definitions selected by
their typed read/write/readwriter annotations. It adds host clock/reset to
each definition and every promoted instance, creates a reset-zero enable
register with `next = finishing ? 1 : enabled`, and connects the shared
`AbstractClockGate` with `CE = enabled & finishing & ~hostReset`. Memory clocks
use that gate's output. The original target-clock input and ancillary instance
clock writes are removed. Scalar data ports and retained annotations stay
unchanged. Models with explicit local clock channels/associations are rejected;
retained annotations targeting the removed clock are also rejected.

This is a partial FAME boundary: `targetCycleFinishing` is reserved for the
subsequent data-channel/FSM rewrite and remains undriven here. This artifact
is not a complete executable FAME model or a default-build SRAM implementation.

`SRAMVirtualClocksOracle.scala` runs the unchanged `FAMEModuleTransformer` on
the independent SFC-prepared models. It resolves reference types/kinds in the
oracle first, as Midas does before clock substitution. The native candidate
is only parsed. The comparison checks all eight enable/finishing/reset input
combinations, register clock/reset/init, gate input, every reader/writer/
readwriter clock, memory specification, scalar data ABI, and host clock/reset
wiring on every promoted instance. Both the four-instance fanout model and
the real Rocket `rf` model derived freshly from the immutable `.sfc.fir` above
match. SFC emits no non-hub generated-clock XDC annotations. The native
annotation output exactly matches its pre-clock preparation output.

CTest additionally checks four host interfaces, removal of all ancillary
SRAM instance clock writes, all three memory port clocks, constant-one
virtual enable, unchanged annotations, and rejection of a protected clock.
Seven focused tests pass. A direct native baseline through PrintBridge host
binding passes; manager validation remains harness-owned.

Artifacts are in the mutable U250 generated directory's
`iteration47-sram-clocks/`: `oracle/golden-rocket.virtual-clock.sfc.fir/json`,
`golden-rocket-clocks/post-sram-clocks.fir` and its annotation sidecar, fanout
equivalents, `clock-comparison.log`, `ctest.log`, and baseline stdout/stderr.
Compile `SRAMVirtualClocksOracle.scala` alongside the preparation oracle files
above, then run:

```sh
for name in fanout golden-rocket; do
  "$native_compiler" "$evidence/oracle/$name.channels-input.fir" \
    --annotation-file "$evidence/oracle/$name.channels-input.json" \
    --output-dir "$evidence/$name-clocks" --rewrite-sram-clocks
done
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMVirtualClocksOracle "$evidence"
ctest --test-dir "$native_build" --output-on-failure \
  -R 'goldengate-(sram-model-channels|fame-virtual-clock-port|fame-clock-gate|fame-clock-gate-identity|fame-output-selection|comb-dependency|fame-channel-clock-domains)$'
```

## Native SRAM FAME data hardware (iteration 48)

`--rewrite-sram-fame` prepares the same wrapped handoff, snapshots native
combinational dependencies and scalar channel identities, then creates each
SRAM's virtual clock, Decoupled data ports, fired registers, ready/valid rules,
and driven `targetCycleFinishing` wire. It refreshes hierarchy and port bindings
between channel rewrites. The model interface places host controls before
inputs and outputs; no explicit clock-token sink is created. Every channel
starts unfired and uses constant-one virtual enable, following SFC
`genMetadata(None)`. Unknown metadata referring to replaced data ports and
repeated SRAM instances are rejected. A verified circuit clone is committed
only after the entire rewrite succeeds.

The initial Rocket candidate exposed a real helper mismatch: ChannelExcision
already named its scalar top ports as the eventual FAME channels. Scalar input
and output helpers now allow reusing the replaced port's name, while preserving
collision rejection for other ports. Model/top metadata transfer and final
MLIR verification still run.

`SRAMFAMEOracle.scala` builds the independent register-file hardware with
unchanged `FAMEModuleTransformer`, using the fresh golden-derived prepared
circuit above. It only parses the CIRCT candidate. It compares all ten channel
ABIs, the SRAM memory specification, payload assignments and gated memory
clocks, host/top channel connections, reset values and these equations:

| Rule | Compared cases |
| --- | ---: |
| Ten fired next-state rules | 160 |
| Eight input ready rules | 128 |
| Two output valid rules, including all nondependent write-input valids | 2,048 |
| Target-cycle completion over every input-valid/output-fired/ready/valid combination | 16,384 |

The comparison passes. The native pre-FAME preparation again matches the SFC
boundary structurally: 100 Rocket annotation records and 197 fanout records.
For annotation transfer, the comparator applies SFC's own `Annotation.update`
to that verified native preparation, using actual top/instance connections.
This preserves the compilers' legal hub-instance spelling differences. The
resulting annotation multiset matches the native output, including SRAM
memory-port metadata and local/global channel targets at `.bits`; transformed
SRAM DontTouch records are consumed as in SFC.

Artifacts are under the mutable U250 generated directory's
`iteration48-sram-fame/`: `oracle/golden-rocket.fame.sfc.fir/json`,
`golden-rocket-fame/post-sram-fame.fir` and its annotation sidecar,
`fame-comparison.log`, and fresh preparation comparisons. Compile
`SRAMFAMEOracle.scala` alongside the existing preparation oracle files, then:

```sh
"$native_compiler" "$evidence/oracle/golden-rocket.channels-input.fir" \
  --annotation-file "$evidence/oracle/golden-rocket.channels-input.json" \
  --output-dir "$evidence/golden-rocket-fame" --rewrite-sram-fame
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMFAMEOracle "$evidence"
```

The comparator also reads `golden-rocket-candidate/post-sram-channels.fir` and
`post-sram-channels-all.json` from the preparation reproduction above. CTest derives a single-instance read/write/readwrite probe from
the existing fanout handoff and checks all thirteen fired registers, finishing,
channel bindings and payload annotation targets. It retains the four-instance
clock test and rejects repeated-instance data rewriting and unknown metadata.
All ten focused native tests pass. The direct default compiler boundary through
PrintBridge host binding is also checked in this iteration. Manager validation
remains harness-owned.

## Shared SRAM FAME definitions (iteration 49)

The data rewrite now transforms a shared SRAM definition once and rewires
all its directly promoted instances. Scalar input/output helpers distinguish
definition transformation from subsequent instance rewrites, validating the
shared definition's channel name, type and stable port index. Final interface
grouping reorders every instance with its definition. Hierarchy analysis retains
shared host clock/reset edges only for matching control names, input directions
and types; ordinary target-port fanout remains an error. The SRAM boundary still
commits only a fully verified circuit clone.

The four-instance test exposed a second single-instance assumption in data
selection: repeated bindings of a local input group collided on the definition's
port index. Selection now shares an input group across distinct instances after
checking ordered payload identity. A second input channel claiming the same
instance remains an error. Fired registers and ready/valid/finishing rules are
constructed once per local channel; every instance retains its own global
channel targets and top bindings.

`SRAMFAMEOracle.scala` now checks both the fresh immutable-golden-derived Rocket
register file and the four-instance read/write/readwrite fanout against the
unchanged `FAMEModuleTransformer`. It compares each definition's memory and
payload/gate wiring, every promoted instance's host/Decoupled bindings, and the
SFC annotation rename multiset. Exhaustive equation comparisons cover:

| Input | Instance bindings | Fired transitions | Output-valid cases | Finishing cases |
| --- | ---: | ---: | ---: | ---: |
| Golden Rocket register file | 1 | 160 | 2,048 | 16,384 |
| Four-instance fanout | 4 | 208 | 16,384 | 131,072 |

Both equation/annotation comparisons pass, and preparation still matches all
100 Rocket and 197 fanout annotation records. All ten focused native tests pass,
including rejection of competing input bindings, ordinary data fanout and
mismatched host-control identities. The direct default compiler through
PrintBridge host binding passes with empty stderr. Manager gates remain
harness-owned.

Artifacts are in the mutable U250 generated directory's
`iteration49-sram-fame/`: the fresh golden-derived preparation oracle,
`oracle/golden-rocket.fame.sfc.fir/json`,
`oracle/fanout.fame.sfc.fir/json`, matching native `*-fame/post-sram-fame.fir`
and annotation sidecars, and preparation/FAME comparison logs. The comparator
requires preparation outputs for both inputs and FAME candidates for both:

```sh
for name in fanout golden-rocket; do
  "$native_compiler" "$evidence/oracle/$name.channels-input.fir" \
    --annotation-file "$evidence/oracle/$name.channels-input.json" \
    --output-dir "$evidence/$name-fame" --rewrite-sram-fame
done
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMFAMEOracle "$evidence"
```

## Parent/SRAM pipe transport (iteration 50)

The opt-in `--rewrite-sram-transport` boundary now constructs both the parent
clock hub's FAME hardware and the promoted SRAM definitions' virtual-clock
hardware, then joins their data endpoints with the native host-clocked pipe
queues and activates the Boolean-clock wrapper. The four-instance probe has
52 internal queues, one SRAM definition and one clock hub. All data channels
stay inside the wrapper; only host controls and the clock packet are external.
The parent input fired states use the raw clock token and reset fired; its
output states use the buffered token and reset unfired. SRAM channels retain
their virtual-clock reset/unfired behavior. The rewrite commits a verified
clone and requires complete scalar integer pipe coverage of the parent.

Clock analysis required last-connect normalization after promotion. The
comparison also exposed promoted wrapper clock aliases whose names differ
from their model ports. Clock-output removal now resolves their direct SSA
connections, including ordinary connects emitted by `ExpandWhens`, instead
of assuming equal names. Eight identity/alias/LowerTypes cases pass, and
19 unsafe plans reject without mutation.

`SRAMPipeTransportOracle.scala` runs the unchanged full SFC `FAMETransform`
and production `SimWrapper` on the independently prepared four-instance input.
The native candidate is only parsed. Its parent/SRAM ABIs, all 67 register
reset/transition rules and 72 ready/valid/finishing/gate equations match after
expanding temporary nodes and canonicalizing Boolean association/constants.
Every queue's payload, valid, ready and host-control connection is checked.
The complete 52-pair endpoint multiset matches both SFC's annotations and
the actual production SimWrapper wiring. Existing pipe-channel behavioral
tests cover the reused queue implementation.

The golden comparison uses the unchanged
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`.
Fresh preparation still matches all 100 Rocket and 197 fanout annotation
records. The extracted Rocket register file still matches SFC's ten Decoupled
ABIs, memory/payload/gate wiring, 160 fired transitions, 2,048 output-valid
cases, 16,384 finishing cases and annotation renames. Its isolated parent
omits non-SRAM channels, so it is not a complete parent transport oracle:
SFC full-parent FAME cannot transform that partial graph, and the native
transport boundary rejects incomplete data coverage. This probe is kept at
the SRAM-only comparison boundary.

All nine focused CTests pass. After rebuilding the shared clock-coupling test
against the changed helper, its four modes and the SRAM boundary pass again.
The direct default compiler through PrintBridge host binding passes with empty
stderr. FireSim manager verification remains harness-owned.

Artifacts are in the mutable U250 generated directory's
`iteration50-sram-transport/`: fresh preparation and SRAM FAME candidates,
`fanout-transport/post-sram-transport.fir` and its annotation sidecar,
`oracle/fanout.transport-fame.sfc.fir/json`,
`oracle/fanout.transport-wrapper.sfc.fir`, and comparison/test logs.
After compiling the Scala oracle alongside the existing preparation oracles:

```sh
"$native_compiler" "$evidence/oracle/fanout.channels-input.fir" \
  --annotation-file "$evidence/oracle/fanout.channels-input.json" \
  --output-dir "$evidence/fanout-transport" --rewrite-sram-transport
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMPipeTransportOracle "$evidence"
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMPipeTransportCompare "$evidence"
```

## Complete golden Rocket parent boundary (iteration 51)

The same immutable `firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` above
now supplies a second probe with every external Rocket data port independently
assigned a scalar pipe channel. The probe exports the hub clock for these
bridge-facing channels. It runs unchanged production SFC preparation,
`FAMETransform`, and `SimWrapper`; no Scala transformation runs on the native
candidate. The historical isolated SRAM probe remains available.

This comparison first exposed retained `firrtl.transforms.CombinationalPath`
metadata without a native transfer policy. SRAM FAME now validates its ground
output sink and ordered input sources, then transfers changed targets to their
Decoupled payloads. Clock-input references also transfer to the clock payload,
matching SFC. CTest checks source order and duplicates, and rejects missing or
input-direction sinks before committing the cloned circuit.

`--rewrite-sram-parent-fame` emits verified parent/SRAM FAME hardware before
queue construction. This is the boundary corresponding to SFC's full
`FAMETransform`. Large completion reductions now use named FIRRTL nodes every
32 conditions when there are more than 64 data channels. This preserves the
ordered AND reduction while avoiding multiline inline expressions that the
pinned SFC parser cannot read. The comparison expands those nodes and checks
all 513 SFC finishing conditions and 512 clock-ready conditions.

`SRAMParentFAMECompare.scala` confirms ten SRAM channel ABIs, every memory
semantic field, 515 common parent ports, and 33 of 39 combinational-path
records including ordered source targets. It explicitly reports two remaining
differences instead of treating this as complete transport equivalence:

- Eight native output aliases still have separate channel ports and completion
  conditions; SFC maps them to the equivalent canonical hub outputs.
- Six path records still refer to scalar top passthroughs (`io_hartid` and
  `io_dmem_resp_bits_data`, and their outgoing aliases). SFC moves these to
  external channel payloads. Full native transport rejects at
  `PipeChannel external_io_hartid requires a passive integer Decoupled payload
  target with matching direction`.

The complete preparation graph has 525 channels in both compilers. Native
local groups/protection records exceed SFC by eight, consistent with the alias
port difference. The four-instance closed transport still matches SFC's 67
register transitions, 72 handshake/completion/gate equations and all 52 queue
endpoint pairs. All ten focused CTests pass after rebuilding the shared
finishing helper. The default native compiler also passes through PrintBridge
host binding with empty stderr. Manager verification remains harness-owned.

Mutable evidence is in `iteration51-rocket-sram-transport/` beneath the U250
generated directory. It includes independent SFC preparation/full-FAME/wrapper
artifacts, `golden-rocket-parent-fame/post-sram-parent-fame.fir` and its sidecar,
`parent-comparison.log`, `fanout-comparison.log`, `ctest-final.log`, and the
full transport rejection in `native.stderr`. Compile the new comparator
alongside the existing Scala oracles, then reproduce with:

```sh
java -Xmx12G -cp "$oracle_classes:$midas_classpath" SRAMModelChannelsOracle \
  "$evidence/oracle" "$immutable_sfc_fir" complete
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMPipeTransportOracle "$evidence" complete
"$native_compiler" "$evidence/oracle/golden-rocket.channels-input.fir" \
  --annotation-file "$evidence/oracle/golden-rocket.channels-input.json" \
  --output-dir "$evidence/golden-rocket-parent-fame" --rewrite-sram-parent-fame
java -Xmx4G -cp "$oracle_classes:$midas_classpath" \
  midas.passes.fame.SRAMParentFAMECompare "$evidence"
```

## Top passthrough transport (iteration 52)

The parent boundary now recognizes direct top input-to-output SSA connects
with unique scalar `PipeChannel` endpoints. It constructs whole Decoupled
connections, forwarding payload and valid toward the output and ready toward
the input. It removes the original scalar ports and transfers their retained
targets to `.bits`, using the existing wrapper annotation/inner-symbol policy.
Analysis precedes port mutation; names survive index changes during model
channelization. Unsupported metadata and generated-name collisions reject the
staged circuit before any candidate FIRRTL is published.

The same immutable Rocket `sfc.fir` listed above was prepared again by the
unchanged Scala passes. Fresh `FAMETransform` and production `SimWrapper`
oracles are in `iteration52-top-passthrough/oracle/` beneath the mutable U250
generated directory. Native candidates are
`golden-rocket-parent-fame/post-sram-parent-fame.fir` with its annotation sidecar
and `golden-rocket-transport/post-sram-transport.fir`.
`parent-comparison-final.log` records:

- All 39 combinational-path annotations match, preserving target identity,
  source order and multiplicity. The six remaining iteration 51 path mismatches
  are resolved.
- Both Rocket top passthroughs match SFC's Decoupled port types and whole-bundle
  connections. The ten register-file channel ABIs and all memory semantic
  fields still match; the 515 common parent ports remain identical.
- Full native Rocket transport succeeds and has 524 queues, matching SFC's
  queue count and payload-type multiplicities. The six passthrough/fanout
  queues match every payload, valid, ready, destination and host-control
  equation, including the shared bridge inputs' ready reductions.

Eight native parent output aliases still have extra channel ports and
completion conditions. This comparison explicitly retains that discrepancy;
queue-count agreement does not establish complete transport equivalence.
All 513 common finishing and 512 clock-ready conditions match after expanding
native reduction nodes.

CTest additionally covers unsigned and signed top passthrough payloads,
duplicate ordered source metadata, unsupported metadata and name collisions.
An initial negative test for raw input fanout was corrected: channel excision
legally gives those branches separate input ports before FAME. Eleven focused
tests and the direct baseline through PrintBridge host binding pass.
Manager verification remains harness-owned.

Reproduce the parent comparison as above, and additionally emit the transport
candidate before running the extended comparator:

```sh
"$native_compiler" "$evidence/oracle/golden-rocket.channels-input.fir" \
  --annotation-file "$evidence/oracle/golden-rocket.channels-input.json" \
  --output-dir "$evidence/golden-rocket-transport" --rewrite-sram-transport
```

## Original output identities and shared bridge queues (iteration 53)

`PromotePassthroughConnections` now retains the original model output reached
through exact SSA wire/node/output aliases. It stops at primitives, memories,
registers (including their next-state connects), and opaque child outputs.
Input passthroughs still traverse hierarchy. Width conversions, ambiguous
drivers, cycles and clock sinks are not promoted. The model body is unchanged;
only wrapper source operands change, so `InferModelPorts` can deduplicate the
same physical output groups as SFC before constructing fired-state hardware.

Fresh Scala preparation of the immutable compiler fixture
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
matches native preparation on all 1,765 annotation records, 525 channels,
523 local groups, 540 top ports and the selected register file's memory
semantics. The channel comparator no longer equates distinct alias ports;
it checks the actual model bindings. The four-instance preparation probe
also matches all 197 annotation records.

The strict pre-queue comparison matches all 515 Rocket port names, directions
and types, all 513 finishing and 512 clock-ready conditions with their
multiplicities, all 39 combinational-path records, both top passthroughs,
and all ten register-file channel ABIs/memory semantic fields. The eight
duplicate alias channels and their additional completion terms are removed.

Transport initially rejected the newly shared sources because the SRAM path
omitted `AddRemainingFanoutAnnotations`. It now invokes the existing native
analysis after FAME renames and before queue construction, matching the
production `MidasTransforms` order. The Scala transport probe was also missing
this step; it now executes the unchanged production pass before `SimWrapper`.
Earlier transport counts remain useful, but did not test the atomic enqueue
equations for these eight alias groups.

`FAMEPipeChannel` now supports multiple bridge channels sharing one target
output. Scala's `ChannelizedWrapperIO` deduplicates their external output;
all queues receive its ready and the final channel in annotation order drives
its bits/valid after last-connect resolution. The native wrapper emits that
single final driver explicitly while preserving all queues and the producer's
atomic ready reduction. A typed SSA regression covers alias identity and
unsafe paths. The fanout interpreter covers both bridge-alias orders with
different queue latencies, reset and backpressure.

The complete Rocket transport comparison now matches all 524 queue payload
types, payload/valid/ready connections, output destinations and host controls,
plus all 367 model-source ready reductions. The four-instance probe matches
all register reset/transition rules and handshake equations and all 52
internal queues. Twelve focused CTests pass, including the new output-identity
test, signed payload/output-clock/fired-state tests, coupled clock/pipe tests
in both channel orders, and pipe fanout tests. A direct default native compile
through PrintBridge host binding also passes. FireSim manager verification
for this change remains harness-owned; iteration 52's supplied replacertl and
Verilator workload results validate the preceding checkpoint.

Mutable evidence is under `iteration53-output-aliases/` in the U250 generated
directory: `oracle/golden-rocket.channels.sfc.{fir,json}`,
`oracle/golden-rocket.transport-fame.sfc.{fir,json}`,
`oracle/golden-rocket.transport-wrapper.sfc.fir`, both native candidates,
`channels-comparison-final.log`, `parent-comparison-final.log`,
`fanout-comparison-final.log` and `ctest-final.log`. No candidate is compiled
by SFC; the comparison parses and inspects native FIRRTL directly.

## Native RAM command adapter (iteration 54)

`RAMModelAdapter` ports the body replacement and command wiring from
`EmitAndWrapRAMModels` through typed CIRCT operations. Its compiler boundary is
`--wrap-ram-model wrapper implementation`, on an already FAME-transformed
circuit containing an elaborated host RAM module. It resolves retained read
and write annotations to scalar bits or one ground field below aggregate bits,
checks uniform unsigned address/data widths and the complete `RegfileModelIO`
interface, then replaces the selected wrapper body in a verified transaction.
All wrapper ports, their order, and retained annotations survive unchanged.

Read command valid combines address and enable valid; each input ready includes
command ready and the other input valid. Write commands combine data, mask,
address and enable with the same rendezvous rule. Response bits, valid and
ready connect directly to the corresponding read output. Host clock/reset
drive the implementation, and its target-reset channel receives constant
valid one and bits zero, matching SFC. Distinct payload fields retain distinct
annotation identities even when they share a channel. Duplicate annotations
select hardware once while remaining in the emitted annotation list.

`RAMModelAdapterOracle.scala` runs unchanged production FAME and
`EmitAndWrapRAMModels` on the recorded independently prepared Rocket probe.
That probe derives from the immutable
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`,
selecting `Rocket.rf`. The candidate uses the previously compared native FAME
wrapper. Both sides share the Chisel async timing-model constructor; only that
constructor is normalized to remove legacy `validif` for CIRCT ingestion.
The native adapter is never compiled or rewritten by SFC. The comparator
inspects the selected module directly, excluding unrelated hub expressions
that CIRCT can wrap beyond the pinned SFC parser's grammar. Command vector
indices are identified by their bound address input because SFC's mutable
annotation set does not specify their order.

All 29 Rocket command/response/handshake/host/reset-token equations match the
SFC wrapper, with two reads, one write, five address bits and 64 data bits.
SFC's constructor therefore uses depth 32 even though the original memory has
depth 31; this is oracle behavior, not an inferred native timing-model change.
An aggregate probe with one read and two writes matches all 23 distinct
equations and all retained annotations, including duplicate records.
`RAMModelAdapterTest.cpp` verifies aggregate replacement and eight atomic
rejections: implementation width mismatch, signed data, unresolved width,
combined readwrite ports, non-bits targets, retained body targets, identical
wrapper/implementation and channels shared between different commands.
Combined readwrite ports are also rejected by the Scala oracle. Cross-command
sharing requires an arbitration policy before it can be supported safely.

The compiler and unit test build successfully; 37 focused SRAM/FAME CTests
pass. Mutable evidence is under `iteration54-ram-adapter/` in the U250 generated
directory: `golden-rocket.expected.fir`, `golden-rocket-native/post-ram-adapter.fir`,
the analogous aggregate artifacts, `compare.stdout`, and `ctest.log`.
The supplied iteration 53 replacertl and Verilator workload results pass;
manager verification of this new boundary remains harness-owned.

## Native async RAM timing model (iteration 55)

`--emit-async-ram module` replaces a selected host module body with typed
CIRCT FIRRTL operations implementing `AsyncMemChiselModel`. It consumes only
its `clock`, `reset`, and aggregate `RegfileModelIO` port ABI. The implementation
emits priority read arbitration, a sampled memory-data register, per-reader
response buffers and four-state controllers, reset-token capture, ordered
write completion, and target-reset/host-reset write suppression. Host reset
flushes protocol state while retaining memory and data buffers. Read-command
`en` is intentionally unused, matching the Scala oracle.

Emission preserves module/port metadata and all retained annotation records.
It rejects malformed types/directions, empty command vectors, unknown widths,
and references into the replaced body (including hierarchical targets) before
mutation. The body is built and verified on a temporary module before transfer.
`AsyncRAMModelTest.cpp` covers repeatable replacement, local/hierarchical port
identity preservation, and ten atomic rejection boundaries.

`AsyncRAMModelOracle.scala` selects the independently generated production SFC
`RamModel` from iteration 54's `golden-rocket.expected.fir` and
`aggregate.expected.fir`. The Rocket reference derives from the immutable
compiler artifact:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

The selected original memory is `Rocket.rf`, `UInt<64>[31]`, with two reads and
one write. Production `EmitAndWrapRAMModels` uses its five address bits to emit
a depth-32 host RAM. A fresh 3-read/3-write, depth-16, 23-bit production Chisel
model adds a wider arbitration probe. Candidates receive empty module bodies
with only the oracle ABI; no SFC transform supplies or rewrites native hardware.

The comparator evaluates each exported FIRRTL operation and requires matching
ABI, memory configuration, register identities, outputs (including invalid
response bits), every register next value, async read address and enabled
writes. Each shape passes 10,000 arbitrary-state transitions followed by
10,000 stateful cycles with memory updates, randomized stalls, host resets and
target-reset tokens: 60,000 transitions total. SFC `validif` write fields are
compared only when the corresponding write is enabled. Unsupported operations
are rejected by the evaluator. The combined Rocket candidate starts from the
native iteration-54 wrapper; its 29 wrapper equations and all 1,249 annotation
records remain unchanged while its host body matches the checked native model.

Mutable evidence is under `iteration55-async-ram/` in the U250 generated
directory: `*.expected.fir`, `*-native/post-async-ram.fir`, `comparison.log`,
`golden-rocket-combined/`, `combined-comparison.log` and `ctest.log`.
The native compiler and unit test build; 41 focused SRAM/FAME CTests pass.
The three standalone native host modules also lower through CIRCT `firtool`
to SystemVerilog. For reingestion, the test removes only CIRCT's `public`
module keyword, which the pinned exporter emits even with a FIRRTL 1.2 header;
the parser rejects that syntax/version pair. This is a compiler-boundary test; manager validation remains
harness-owned.

After sourcing the FireSim environment, compile `AsyncRAMModelOracle.scala`
with the pinned Scala 2.13 compiler and Midas runtime classpath, then:

```sh
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.AsyncRAMModelOracle \
  "$evidence" "$iteration54_evidence"
for name in golden-rocket aggregate multiport; do
  "$native_compiler" "$evidence/$name.input.fir" \
    --annotation-file "$evidence/$name.input.json" \
    --output-dir "$evidence/$name-native" --emit-async-ram RamModel
done
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.AsyncRAMModelCompare "$evidence"
"$native_compiler" "$evidence/combined.input.fir" \
  --annotation-file "$evidence/combined.input.json" \
  --output-dir "$evidence/golden-rocket-combined" --emit-async-ram RamModel
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.AsyncRAMModelCombinedCompare \
  "$iteration54_evidence" "$evidence"
```

## Native RAM module materialization (iteration 56)

`--materialize-ram-model wrapper` completes the selected post-FAME
`EmitAndWrapRAMModels` boundary without any supplied host declaration or Chisel
body. The shared adapter analysis resolves retained memory annotations to typed
channel ports and derives address/data widths and distinct read/write commands.
CIRCT creates the complete `clock`, `reset`, `RegfileModelIO` ABI, emits the
native async timing model, and replaces the selected wrapper with the verified
adapter. Host names use the circuit namespace, including external modules;
each materialization owns separate storage. Annotation records, wrapper ports,
module metadata and other module bodies are preserved.

The host is owned temporarily until both body emissions succeed. Unsupported
widths or wrapper-body identities fail without leaving a new host or replacing
any wrapper operations. The adapter now also rejects attached body annotations
and hierarchical targets through erased body instances, while preserving local
and hierarchical references to surviving wrapper ports. The expanded C++ test
passes host-free creation, internal/external name collisions, repeated creation
with independent storage, unchanged other modules and fifteen atomic rejections.

The fresh Scala oracle uses production `EmitAndWrapRAMModels` on the same
Rocket register-file boundary derived from the immutable compiler artifact:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

`Rocket.rf` is `UInt<64>[31]` with two reads and one write. Its resolved five-bit
address interface produces a depth-32 host RAM in both compilers. The native
candidate receives the native post-FAME wrapper and retained annotation stream,
with no `RamModel` module in its input. Its 29 adapter equations match the fresh
SFC boundary by bound address identity, its wrapper ABI is preserved, and all
1,249 annotations match exactly. A duplicate-annotation aggregate case with
one read and two writes likewise matches 23 equations and all six annotations.
Both combined probes also match the indexed address-to-command bindings, so
their host priority order agrees with SFC.
The exported native host operations pass 20,000 differential transitions per
shape against SFC, including arbitrary complete states, stateful memory updates,
backpressure, host reset and target reset: 40,000 transitions total. Every
output, register D, read address and enabled write matches. Both complete
candidate hierarchies lower through CIRCT `firtool` to SystemVerilog, including
native `RamModel`, wrapper instances and `data_32x64` / `data_8x17` storage.
Reingestion removes only the pinned exporter's unsupported FIRRTL-1.2 `public`
keyword, as described in iteration 55.

The compiler and focused unit test build, and 41 SRAM/FAME CTests pass. Mutable
evidence is in `iteration56-ram-materialization/` under the U250 generated
source directory: host-free `*.input.fir/json`, fresh `*.expected.fir`,
`*-native/post-ram-model.fir`, `post-ram-model-all.json`, `rtl/`,
`adapter-comparison.log`, `timing-comparison.log`, `unit.log` and `ctest.log`.
The supplied iteration-55 CIRCT replacertl and required Verilator regression
gates pass; manager verification of this new optional boundary is harness-owned.

After sourcing the FireSim environment and compiling both Scala oracle files:

```sh
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.RAMModelAdapterOracle \
  "$evidence" "$iteration53_evidence" --native-host
"$native_compiler" "$evidence/golden-rocket.input.fir" \
  --annotation-file "$evidence/golden-rocket.input.json" \
  --output-dir "$evidence/golden-rocket-native" --materialize-ram-model rf
"$native_compiler" "$evidence/aggregate.input.fir" \
  --annotation-file "$evidence/aggregate.input.json" \
  --output-dir "$evidence/aggregate-native" --materialize-ram-model Aggregate
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.RAMModelAdapterCompare \
  "$evidence" --materialized
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.AsyncRAMModelCompare \
  "$evidence" --materialized
```

## Complete optional SRAM timing/transport boundary (iteration 57)

`--rewrite-sram-models` now carries one prepared CIRCT circuit through parent
and SRAM FAME, pipe transport, native async RAM materialization, and XDC
resolution. `rewriteSRAMTimingModels` discovers selected definitions from typed
memory-port targets and performs the entire rewrite on a verified circuit
clone. Unsupported timing ABIs or late XDC failures discard that clone. The
CLI emits `post-sram-models.fir`, its complete annotation sidecar, and the two
`post-sram-models` XDC files.

Hub controls already attached generated-clock metadata to actual gate
operations. Exporting and re-importing FIRRTL between the earlier isolated
boundaries lost those attributes. The combined boundary resolves them after
transport insertion and SRAM body replacement, before text export. Virtual
SRAM gates disappear with the replaced bodies; only surviving hub gates
contribute constraints. CTest checks four promoted SRAM instances, MFMR=3
setup and MFMR-1 hold, comment-only synthesis collateral, unsupported readwrite
ports and a late missing-path failure. The existing readwrite fixture remains
unchanged; its supported timing probe removes those ports in a mutable copy.

The exact immutable source is
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`.
It contains Rocket's `rf : UInt<64>[31]` at line 145182. Fresh production SFC
preparation selects that register file and complete Rocket external channels.
`SRAMTimingModelsOracle.scala` independently runs unchanged SFC preparation,
FAMETransform, EmitAndWrapRAMModels and WriteXDCFile. No supplied host module
or SFC-transformed model is ingested by the native candidate.

The structured comparison passes for 21 adapter equations, 61 hub control
equations, four instances and 72 retained annotations on the fanout probe;
Rocket matches 29 adapter equations, 1,029 hub control equations, one instance
and 1,248 retained annotations. Hub/adapter ABIs, generated FAME register
contracts, memory command indices and both XDC bodies match. Annotation
normalization accounts only for circuit/container insertion, legal hub-instance
uniquing, and conversion of the scalar Clock token to its Boolean vector lane.
The XDC oracle supplies the outer transport container as a path prefix, then
SFC independently expands the actual hub/gate hierarchy below it. Ordinary
Rocket register-expression serialization still differs between normalization
pipelines; this comparator checks generated FAME controls rather than claiming
complete target RTL equivalence.

`AsyncRAMModelCompare --sram-timing` evaluates the actual native host operations
from these complete candidate circuits against SFC over 40,000 transitions,
including arbitrary register states, reset, backpressure, responses and writes.
All 39 focused SRAM/RAM/FAME/XDC CTests pass. Both complete candidate hierarchies
lower through pinned CIRCT firtool to split SystemVerilog; the mutable lowering
copies remove only the exporter's public-module keyword for FIRRTL 1.2 parser
compatibility.

Direct comparison with the immutable U250 build-tree
`design/FireSim-generated.implementation.xdc` confirms the same host-clock
source, divide-by=1, setup=1 and hold=0 contract. Clock names and gate paths
intentionally differ because the probe isolates Rocket instead of the full
FireSim hub. `design/FireSim-generated.synthesis.xdc` matches the comment-only
native synthesis body exactly. The fixture root is
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01`.

Mutable evidence lives under `iteration57-sram-timing-models/` in the U250
generated directory: preparation and RAM/XDC oracle outputs, complete native
candidates, `comparison.log`, `timing-comparison.log`, `golden-clock-contract.log`,
`ctest.log` and both lowering logs/RTL directories. After sourcing the FireSim
environment and compiling the Scala oracle files, reproduce with:

```sh
java -Xmx8G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.SRAMTimingModelsOracle "$evidence"
"$native_compiler" "$evidence/golden-rocket.input.fir" \
  --annotation-file "$evidence/golden-rocket.input.json" \
  --output-dir "$evidence/golden-rocket-native" --rewrite-sram-models
"$native_compiler" "$evidence/fanout.input.fir" \
  --annotation-file "$evidence/fanout.input.json" \
  --output-dir "$evidence/fanout-native" --rewrite-sram-models
java -Xmx8G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.SRAMTimingModelsCompare "$evidence"
java -Xmx4G -cp "$oracle_classes:$midas_classpath" midas.passes.fame.AsyncRAMModelCompare "$evidence" --sram-timing
```

## Ordered parent payloads through SRAM transport (iteration 58)

The parent FAME rewrite now accepts ordered groups of ground UInt/SInt ports.
It resolves each live port again after interface mutation and packs the group
with the existing native channel operations. Retained top/model references
move to individual `bits` fields using SFC `hostDecouplingRenames`' common-prefix
rule. Scalar SRAM commands and shared SRAM definitions retain their previous
ABI. The boundary still commits only a verified circuit clone.

The first candidate reached queue construction but failed because its ordered
field references were interpreted as several incomplete payloads.
`FAMEPipeChannel.cpp` now accepts a complete, ordered list of immediate integer
fields on the same Decoupled payload. One group creates one queue and transfers
its entire bundle with one ready/valid handshake. Incomplete, duplicate,
reordered and nonpayload field lists fail before queue creation. Whole-payload
targets retain their existing aggregate/vector support.

Fresh preparation again reads the exact immutable compiler artifact
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`.
The independent SFC oracle groups five real Rocket interrupt inputs and two
instruction-request outputs in reversed order in a mutable annotation copy.
The grouped candidate matches both payload ABIs, all 1,234 annotation records,
1,019 generated hub control equations, 29 RAM adapter equations, generated
register contracts, and both XDC files. This is a grouped-channel probe derived
from the recorded target, not a claim that its original configuration selects
these groups or optional SRAM models. Ordinary target RTL equivalence remains
outside this comparator.

The original fanout and scalar Rocket comparisons still pass. Native RAM
operations from those complete candidates match SFC over 40,000 transitions.
The complete grouped Rocket hierarchy lowers through pinned CIRCT firtool to
split SystemVerilog, removing only the public-module keyword in a mutable
FIRRTL-1.2 lowering copy. Forty focused SRAM/RAM/FAME/pipe/XDC CTests pass.
They include mixed signed/unsigned reversed payloads, one queue per group,
ordered dependency targets with duplicate sources, unsupported leaf metadata
rejection, and malformed queue-field lists. Manager gates remain harness-owned.

Evidence is under the mutable U250 generated directory's
`iteration58-sram-parent-payloads/`: fresh preparation inputs, independent
`grouped-rocket.expected.fir/json`, corresponding native output and `rtl/`,
`comparison.log`, `timing-comparison.log`, `ctest.log`, and lowering/build logs.
Use the iteration-57 commands above with this directory; additionally invoke
`--rewrite-sram-models` on `grouped-rocket.input.fir/json`. The updated oracle
and comparator include that probe automatically.

## Iteration 59: ready/valid parent transport

The optional parent/SRAM boundary now constructs native ReadyValidChannel
operations after the pipe wrapper and before clock normalization/activation.
Forward target-valid and reverse target-ready remain distinct from the host
token handshakes. Pair payload normalization and retained endpoint transfers
use the same native implementation as the baseline FireSim compiler. Internal
SRAM command/response channels retain their pipe transports. Unpaired boundary
names fail on the staged circuit without publishing a partial timing model.

Fresh SFC preparation reads the immutable compiler artifact
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`.
Its Rocket inputs match the previously recorded preparation. A mutable
annotation copy groups actual `io_dmem_req` leaves into an outgoing pair and
`io_imem_resp` leaves into an incoming pair. The unchanged production SFC
preparation, FAME, RAM replacement, SimWrapper and WriteXDCFile generate the
independent reference; no Scala transform compiles native candidate hardware.

The candidate matches 40 flattened SimWrapper port direction/type contracts,
all 32 wrapper handshake/clock/reset bindings, 974 parent FAME control equations,
generated FAME register contracts, all 1,175 retained annotation records, the
29 RAM adapter equations and both XDC files. Wrapper payload identities are
compared after undoing the separately verified SimWrapper payload nesting.
SFC's hash-based read-port collection changes read-lane order for this probe;
the comparator requires a bijection by address-channel identity and transfers
the same permutation to both command and response equations. It does not
discard either half of a lane or compare only unordered port widths.

The complete Rocket candidate lowers through pinned CIRCT firtool to split
SystemVerilog. Focused CTests cover mixed signed/unsigned pair payloads in both
orientations, preserved internal pipe multiplicity, and atomic unpaired-name
rejection, alongside the existing ready/valid queue behavior tests. Evidence
is in the mutable U250 generated directory's `iteration59-sram-ready-valid/`:
`oracle-fresh/`, `oracle/ready-valid-rocket.wrapper.sfc.fir`, independent
`ready-valid-rocket.expected.fir/json`, corresponding `ready-valid-rocket-native/`
including `rtl/`, `comparison-final.log`, `ctest-final.log` and build logs.
The iteration-58 scalar/grouped and shared-memory comparisons still pass.
FireSim manager verification remains owned by the harness.

## Iteration 60: selected memories in full compiler assembly

The normal `--compile-baseline` route now consumes retained
`FirrtlMemModelAnnotation` requests after WrapTop, labels memories before
ExtractModel, and lowers the new interfaces before channel preparation. Selected
memories use the native parent/SRAM FAME, pipe and ready/valid transport, and RAM
replacement. This route then enters the same bridge, host, platform, header and
RTL stages as the unselected compiler. Debug-host combinations fail explicitly
until their selected-memory assembly is supported.

`rewriteSRAMTimingHardware` commits verified hardware while retaining native
clock-gate identity and constraint snippets. It defers XDC resolution until the
host hierarchy is finalized. The standalone `--rewrite-sram-models` route keeps
its existing immediate-XDC contract. A focused `--rewrite-sram-hardware` probe
accepts a missing circuit path and emits hardware identical to the constrained
boundary, without generating premature XDC. Its FIRRTL text is a comparison
artifact; only the live MLIR circuit preserves native gate attributes.

The full selected candidate reads the exact immutable compiler fixture
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
and its `.anno.json`, adding only `~FireSim|Rocket>rf` memory selection in a
mutable annotation copy. It emits simulator SystemVerilog with one native SRAM
definition, the async RAM implementation and complete FireSim host assembly.
An unselected compile from the original annotations also succeeds. Their
`.const.h`, `.const.vh` and `.defines.vh` match.

The independently regenerated Scala reference derived from that exact FIRRTL
matches the full assembled SRAM adapter on 32 port contracts and 32 bindings
and equations. `SRAMCompilerAssemblyOracle.scala` exports the SFC adapter with
its RAM implementation represented by its independently generated port ABI;
pinned firtool lowers that reference. `SRAMCompilerAssemblyCompare.py` compares
port widths/directions, every live implementation binding and handshake
equation, expanding aliases and canonicalizing Boolean conjunctions. The
unused reset-ready output must remain unobserved. This comparison does not
claim equivalence of ordinary target logic or RAM internal state.

Both final XDC files match the immutable U250 build-tree artifacts
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.implementation.xdc`
and `FireSim-generated.synthesis.xdc`, normalizing only SFC's
`target/FireSim_` to native `target_FAMETop/FireSim`. Clock names, ratios and
multicycle setup/hold values match; the replaced SRAM contributes no stale gate.
The four scalar/shared/grouped/ready-valid Scala comparisons still match their
annotation records, FAME control equations, adapter equations and XDC. Forty-two
focused CTests pass after correcting the deferred-XDC test's initial assumption
that native snippet attributes appear in exported annotation JSON.

Evidence is in the mutable generated directory's `iteration60-sram-compiler/`:
`selected-full/`, `unselected-full/`, `boundary/`, `comparison.log`,
`assembly-comparison.log`, `collateral-comparison.log`, `ctest.log` and
`ctest-fixed.log`. To reproduce the final adapter comparison, compile
`SRAMCompilerAssemblyOracle.scala` against the existing MIDAS classpath, invoke
it with `boundary/golden-rocket.expected.fir` and `sfc-adapter.fir`, lower that
file with pinned firtool, then run:

```sh
python3 sim/goldengate-circt/test/Transforms/SRAMCompilerAssemblyCompare.py \
  "$evidence/sfc-adapter.sv" "$evidence/selected-full/FireSim-generated.sv"
```

The iteration-59 manager gate timed out despite the local target UART recording
all 90,141 portable checks, signature `0x78194504c338c229`, and exit status zero.
Complete supplied stdout/stderr and manager logs contain repeated screen-status
polling; an unrelated SPEC `fsim0` screen session was still present. This
supports a completion-monitoring diagnosis, not a target RTL failure. The
harness gate remains failed, and no manager action or unrelated session was
modified. Selected-memory runtime verification remains pending with the harness.

## SRAM FAME debug target transfer (iteration 61)

The native SRAM FAME boundary now transfers
`midas.InternalFirrtlFpgaDebugAnnotation` selections from replaced scalar
ports to their channel payload fields. It uses the same validated
`transferFAMEPortDebugTargets` helper as the single-hub compiler path. Modern
ReferenceTarget and legacy ComponentName spellings retain their respective
formats. Only the schema's target member is transferred; extra members on an
affected debug annotation are rejected before publishing the staged rewrite.
Top passthrough selections are transferred after their payload ports exist
and before queue construction changes the circuit identity.

The unchanged SFC `FAMETransform.hostDecouplingRenames` is the reference.
Before this fix, the legacy selections survived native compilation with names
of erased ports; modern selections were rejected as unsupported metadata.
The new `SRAMDebugTargetsOracle.scala` independently runs SFC preparation and
FAME on mutable Rocket inputs. It first checks their port and memory contracts
against the immutable compiler artifact:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

Both the scalar and grouped Rocket cases match all six SFC probe identities,
duplicate coalescing, and resolved integer payload types. Wrapper selections
are compared through their actual instance connections, allowing different
generated top port names. Native Rocket FAME hardware is unchanged from the
pre-fix candidate. The SRAM channel CTest also checks modern/legacy memory
port selectors through LowerTypes, signed top passthrough selections, unchanged
hardware, and rejection of malformed debug metadata. All five related SRAM,
FAME annotation, LowerTypes and AutoILA CTests pass.

Evidence is in the mutable U250 generated directory's
`iteration61-sram-debug/`: `build-tool.log`, `before-comparison.log`,
`before-modern.log`, `comparison-golden.log`, `ctest-related.log`, and the
independent expected FIRRTL/annotation outputs. Compile the new oracle against
the MIDAS runtime classpath and invoke it with the evidence directory and the
immutable `.sfc.fir` path as its two arguments. No Scala transform runs on
the native candidate.

The complete iteration-60 timeout logs show the bare target passed with
checksum `0x4d81038d93425513`, exited with status zero, and emulated 35,330
cycles in two seconds. Manager polling continued to report the unrelated
SPEC17 `fsim0` session. Local UART and the simulator stderr tail are preserved
in the evidence directory. The harness gate remains failed; no manager action
or unrelated process was modified.

## Selected SRAM AutoILA compiler assembly (iteration 62)

The full compiler now permits `--enable-autoila` with retained memory model
selections. It executes the native SRAM hardware path and the shared host
AutoILA phase, including debug target lowering, hierarchy routing, wrapper/IP
generation, host-clock wiring, driver header capture, hierarchy normalization
and final RTL/XDC emission. The previous unconditional rejection prevented
this supported composition. The selected-memory Print-host guard remains;
its binding path still depends on the single-model assembly.

The complete local compile starts from this immutable compiler artifact:

```
/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir
```

Only the mutable annotation input adds `~FireSim|Rocket>rf` and three public
debug selections: legacy `FireSim.Rocket.reset`, modern
`~FireSim|Rocket>io.imem.req.bits.pc`, and modern
`~FireSim|Rocket>io.imem.resp.bits.data`. The register file has 31 physical
64-bit entries, two readers and one writer. Rocket remains inside the FireSim
FAME model in this configuration; these local probe ports retain their
identities. They are not the channelized Rocket-model ports of the separate
iteration-61 comparison. Probes on rewritten model interfaces continue to use
the validated payload-target transfer from that iteration.

`SRAMAutoILAOracle.scala` independently lowers the immutable target with SFC,
selects the same three Rocket ports, and runs the unchanged SFC AutoILA phase.
The target handoff predates host assembly, so the oracle adds only a top-level
host clock input/source for that host phase. No Scala pass transforms the
native candidate. All three source identities, ordering and widths (1, 40,
32 bits) match, as do all ILA IP settings, with depth 2048 and four comparators.
This is a source/AutoILA-phase comparison; it does not claim complete SFC
selected-memory simulator equivalence.

`SRAMAutoILACompare.py` follows the emitted RTL assignments and instance
bindings through the normalized hierarchy. All three wrapper inputs reach
their selected Rocket port values, and the sampling clock reaches the F1Shim
host clock. The wrapper retains its `SYNTHESIS` guard. Deliberately substituted
reset data, reset in place of the host clock, and a narrowed probe are rejected.
The independently generated SFC adapter reference from iteration 60 also
matches all 32 adapter port contracts and 32 bindings/equations in this full
candidate. The three driver/interface headers and both XDC files are unchanged
from the selected-memory compile without AutoILA.

Both final XDC files also match the immutable U250 reference under
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/`
after mapping the SFC `/target/FireSim_/` hierarchy to native
`/target_FAMETop/FireSim/`. No other constraint normalization is applied.

The SRAM channel CTest now carries three retained parent selections through
four SRAM-model instances and checks twelve ordered ILA routes, source widths,
host-clock binding, private annotation cleanup and IP probe count. Its
standalone ILA import includes the live debug/host-clock annotations rather
than the historical pre-FAME channel clock archive. All eight related SRAM,
FAME annotation, retained-target lowering, AutoILA, host hierarchy, header and
XDC CTests pass.

Evidence is in the mutable U250 generated directory's
`iteration62-sram-autoila/`: `selected-debug.json`, `selected-full.stdout.log`,
`selected-full.stderr.log`, `oracle-comparison.log`,
`rtl-routes-comparison.log`, `rtl-mutations.log`, `adapter-comparison.log`,
`header-xdc-comparison.log`, `golden-xdc-comparison.log`, and
`ctest-related.log`. Compile the Scala oracle against the MIDAS runtime
classpath, then invoke it with the evidence directory and the immutable
`.sfc.fir` path. Run the RTL comparator with `selected-full/`. Finished large
MLIR snapshots are compressed; model, SRAM and normalized hierarchy
boundaries remain directly readable. No manager verification step was started.

The supplied iteration-61 harness gates passed: bare smoke, the 90,141-check
portable suite and the 90,805-check UART-bearing Rocket suite. The latter still
lacks its SFC UART-bearing differential baseline. These gates precede the new
selected-memory AutoILA candidate; runtime verification of this combination
remains pending with the harness.

## Selected SRAM with queued Print host binding (iteration 64)

The explicit `--compile-baseline --stop-after-print-host-binding` boundary now
supports selected SRAM models. After native SRAM FAME, command/response queues
and timing implementations activate `GGFAMEPipeWrapper`, the compiler runs the
existing CIRCT Print payload, token, control, queued-host and constructor-binding
passes on that active wrapper. Print payloads therefore come from its external
queued Decoupled endpoints. The SRAM queues and implementation remain internal.
The earlier selected-SRAM Print rejection has been removed.

The candidate uses the immutable compiler handoff
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
with a mutable annotation copy selecting `~FireSim|Rocket>rf`,
`~FireSim|Rocket>printf_1`, and `~FireSim|UARTTx>printf`. A duplicate Rocket
printf selection checks deduplication. `SRAMPrintBindingOracle.scala` runs the
unchanged Scala PrintSynthesis, FAME and RAM-model passes independently on this
same handoff, emitting pre-FAME and post-RAM FIRRTL/annotation references.
Only the SFC reference's legacy `validif` expressions are removed for import by
pinned firtool; Scala never transforms the candidate.

`SRAMPrintBindingCompare.py` checks the fresh SFC reference against the emitted
native annotations: one Print domain, 18 scalar latency-zero Pipe channels,
formats, argument order within each record, field widths, channel mappings and
rational clock metadata match. It follows emitted native RTL through the
active transport wrapper and queued Print host: host valid requires all 18
tokens, each leaf ready requires host ready and every other leaf valid, and each
payload uses the corresponding queued endpoint. Host clock/reset and all 18
pipe output bindings match these contracts.
Deliberately substituted host clock, payload and leaf-ready equations are
rejected by the RTL comparison.

The native outgoing queue's 6144 x 512 memory and 13-bit occupancy count match
`Queue_50` in the immutable U250 artifact
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
This comparison checks geometry, not complete queue transition equivalence.
The earlier independently generated SFC adapter reference
`iteration60-sram-compiler/sfc-adapter.sv` still matches all 32 Rocket.rf port
contracts and 32 adapter bindings/equations in this candidate. The fresh full
SFC RTL optimizes unused implementation inputs and permutes its two reader
indices, so it is not used for that literal adapter ABI comparison.

There is an observed Print record-order mismatch: SFC places UART before
Rocket, while native synthesis places Rocket before UART. Record contents match
by identity, but their packed offsets and ordered decoder collateral do not yet
match. `comparison.json` explicitly reports `record_order_matches: false`.
The next investigation is SFC's record traversal order and the corresponding
ordered payload offsets and emitted decoder records (see iteration 65 below).

The SRAM channel CTest now carries three Print leaves through four RAM-model
instances, activates their queues, binds a native queued Print host, and checks
retargeted sources/sinks and decoder format. The SRAM, Print token-stage,
control, host and binding CTests pass. The full selected Rocket candidate and
independent Scala oracle also compile successfully. Evidence is in the mutable
U250 generated directory's `iteration64-sram-print/`: `selected-print.json`,
`candidate/`, `sfc-prepared.fir/json`, `sfc-post-ram.fir/json`, `oracle.log`,
`comparison.log/json`, `adapter-comparison.log`, and `ctest-integration.log`.
Reproduce the comparisons with:

```sh
python3 sim/goldengate-circt/test/Transforms/SRAMPrintBindingCompare.py \
  "$evidence" "$immutable_u250/design/FireSim-generated.sv"
python3 sim/goldengate-circt/test/Transforms/SRAMCompilerAssemblyCompare.py \
  "$prior_evidence/sfc-adapter.sv" "$evidence/candidate/post-print-host-binding.sv"
```

This is an explicit compiler stop boundary. Complete Print MMIO/stream platform
assembly and runtime verification of the selected-SRAM/Print combination remain
pending. The supplied iteration-63 manager gates passed the bare smoke,
90,141-check portable suite and 90,805-check UART-bearing Rocket suite; they
precede this optional candidate. The latter suite's SFC UART-bearing baseline
is still pending. No manager verification step was started by the agent.

## Iteration 65: Print source groups and ordered decoder checks

`BridgeTopWiring.scala` groups TopWiring mappings by their pathless source before
expanding absolute instances. Its immutable map determines the order *between*
source groups; PrintSynthesis then preserves that sequence within each clock
domain. Reversing the Rocket/UART vector would fit one fixture without porting
this grouping behavior.

`PrintWiring.cpp` now groups completed output annotations by their native stub
identity. Source groups follow module/statement order; absolute instances within
each group retain hierarchy traversal order. The constructor, channel sequence,
payload operations and decoder all consume that grouped sequence. Data routing,
port identities and clock resolution still use CIRCT operations.

The shared-module native test has one top printf and two Leaf printfs, each
replicated through `left/l`, `right/l` and `direct`. Previously its output sequence
interleaved message/empty per instance. It now keeps all three message replicas
together and all three empty replicas together, including in the emitted bridge
constructor. `PrintSourceGroupingOracle.scala` independently executes the SFC
BridgeTopWiring pass on the corresponding hierarchy: seven outputs, contiguous
source groups, and the same three-instance order within each group. Inter-group
hash ordering differs and is not claimed to match.

A fresh selected-SRAM/Print native candidate and independent SFC reference use
the same immutable `.sfc.fir` and explicit selections recorded above. Evidence
is in `iteration65-print-source-groups/` in the mutable U250 generated directory:
`candidate/`, `selected-print.json`, `sfc-post-ram.fir/json`,
`sfc-shared-print-groups.json`, native/SFC stdout/stderr, `comparison.json`, and
`mutation-results.json`. The 18 channels, constructor fields, formats, clocks,
queued joins and payload routes match by identity. Queue_50 geometry still
matches the immutable U250 `design/FireSim-generated.sv` named above.

`SRAMPrintBindingCompare.py` now checks the emitted RTL concatenation and the
ordered decoder records rather than inferring correctness from an unordered
constructor comparison. Native Rocket/UART offsets are 1/372 with record widths
371/17; SFC UART/Rocket offsets are 1/18. Native argument slices and decoder
offsets match one another and the SFC record contents; raw inter-group byte
layout remains different (`record_order_matches: false`). Deliberately swapped
UART argument slices and a decoder offset of 371 instead of 372 are rejected.

Five focused CTests pass: SRAM channels, Print wiring, payload, queued host and
binding. The native compiler and both independent Scala reference programs
also pass. Supplied iteration-64 manager and Rocket regression gates passed;
these precede this optional candidate. No manager verification step was run.
The next smallest integration step is allocating the bound Print host's six
MMIO words and outgoing CPU stream in full platform assembly. Compatibility
with an SFC decoder's hash-ordered record ABI remains a separate limitation;
the native decoder must accompany its native RTL.

## Iteration 66: bound Print queues enter native CPU transport

`PrintBridgeConfig.cpp` now exposes the six implemented configuration words in
the shared `goldengate.mmioRegisters` registry. The richer Print decoder layout
is retained. `deriveControlMMIOWidget` validates this registry against the actual
six-lane MCRFile type; `allocateControlMMIORegions` therefore allocates 32 bytes
per Print bank, including equal-sized banks in caller registration order.

`mapPrintBridgeCPUStreams` resolves the active top's bound Print host collection,
its instantiated hosts and control ports. It derives stream names using
`StreamToHostCPU`'s uppercase widget-name convention and takes queue geometry
from each host. Explicit caller-supplied streams precede the Print collection;
Print order comes from the binding registry. It connects the actual queued
outputs to `addCPUStreamRead`'s native AXI AR/R transport and their live counts
to `addCPUStreamCountBank`'s read-only MCR words. Both transformations are staged
on a clone and committed together, preserving existing operation identities.

The compiler option `--map-print-host-cpu-streams`, used with
`--compile-baseline --stop-after-print-host-binding`, exercises this stage on
the selected-memory path. It emits `post-print-cpu-streams.mlir`, `.sv` and
`-all.json` after retaining the preceding binding/decoder artifacts. This stop
boundary materializes only Print hosts; its stream zero is not a claim about
the complete platform's bridge registration order.

Evidence is in `iteration66-print-cpu-allocation/` under the mutable U250
generated directory. The candidate and a fresh independent SFC reference use
the immutable compiler oracle
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
with the recorded explicit rf/printf selections. `SRAMPrintBindingCompare.py`
passes again for all 18 Print channels, constructors, clocks, queued joins,
ordered native payload/decoder slices and the immutable queue geometry. Native
versus SFC inter-group record order still differs and is reported separately.

`PrintCPUAllocationCompare.py` compares the emitted transport with
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
The recorded configuration has no Print host. Its `CPUManagedStreamEngine`
transports TracerV using the same stream ABI: after resolving internal aliases,
AR ready, R valid and queue ready have identical handshake terms. The 19-bit
window selection, nine-bit burst counter, 512-bit data/ID forwarding, and
zero-extended 13-bit occupancy also match. The candidate's emitted queue and
count instance connections are checked directly. Removing last-beat gating
from AR ready and detaching the count input are both rejected; results are in
`cpu-allocation-comparison.json` and `cpu-mutation-results.json`.

Five focused CTests pass: Print config, host, binding, control address decode
and multiple CPU reads. The binding test now derives two Print streams after
TracerV through the production consumer, validates ordered count identities
and 512KiB windows, and checks two six-word banks allocate 32-byte regions. Eight
malformed allocation cases leave the circuit unchanged, including a bad count
port discovered after read hardware has been staged. The compiler and the
fresh selected SFC oracle pass. No manager verification step was run.

Global control dispatch, CPU write/response assembly and final driver allocation
remain pending for this optional boundary. The next smallest step is binding
the validated Print bank(s) and CPU count bank into the platform's shared MMIO
dispatch, with complete bridge registration order supplied explicitly.

## Remaining scope

Shared SRAM definitions now have a combined native FAME, transport, timing-model
and generated-clock collateral boundary. Explicit retained memory selections now
execute through full FireSim compiler assembly; the ordinary configuration
selects no optional memories. Selected-memory Rocket runtime verification
remains pending. Legacy retained domain-clock annotations have
the same erased top-clock references as SFC after FAME; later consumers must
use captured domain identity. Selected-memory Print hosts now bind through the shared queued transport at
the explicit compiler stop boundary. Ordered Print records and complete Print
MMIO/stream platform integration remain incomplete. Selected-memory AutoILA runtime verification remains owned by
the harness.
Ready/valid top passthroughs and inter-model
ready/valid endpoints remain unsupported at this optional boundary.
Readwrite/wider memory shapes and FAME-5 remain separate incomplete work.

## Iteration 67: selected Print banks enter native MMIO dispatch

`mapPrintBridgeControlDispatch` composes the implemented Print six-word banks
and CPU stream occupancy bank with native U250 control transport. It resolves
Print identities through the bound host registry, checks the actual queued
host instance and exposed control bundle, and derives each allocation from its
implemented MCR registry. The CPU count adapter uses the allocated bank's word
count. Widget.scala's decreasing-size allocation assigns the Print banks before
the smaller count bank, retaining registration order for equal-sized banks.

`bindControlWidgetWrites` now accepts an explicit implemented widget/port
catalog. It resolves each slave by allocated widget identity and validates
unique nonempty names/ports before mutation. The existing seven-widget entry
point delegates to this API. The error endpoint accepts an explicit expected
top while retaining its existing LoadMem entry point and clock/reset checks.
The new composition stages the complete adapter, error endpoint, address
decoder, queued write route, AW/W binding and AR dispatch on a clone and verifies
it before moving only newly created modules into the original circuit.

Use `--map-print-host-control` with `--compile-baseline
--stop-after-print-host-binding --map-print-host-cpu-streams`. It emits
`post-print-control-dispatch.mlir`, `.sv` and `-all.json`. This boundary consumes
the bound banks' AW/W/AR request interfaces and exposes their B/R responses,
transaction-tracker readiness and master request fields. It does not assemble
a complete platform control bus or claim response/driver integration.

Evidence is `iteration67-print-control-dispatch/` in the mutable U250 generated
source tree. Its selected Rocket/SRAM candidate ingests the immutable oracle
`/scratch/jfx/fsim-circt/sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
with the iteration 66 rf/printf selections. The resulting one Print host has
six words in a 32-byte region at zero, followed by one read-only count word in
a four-byte region at 32. Existing raw annotation classes, payloads and order
are preserved through the composed target transfers.

`PrintControlDispatchCompare.py` compares the candidate's RTL with the immutable
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
Eight normalized `NastiRouter` AW/W/AR dispatch, tracker-enqueue and master-ready
predicates match. Request payload forwarding also matches. The test follows
actual instance nets from the native dispatchers to Print/count controls and
from the live occupancy word to its AXI adapter. This SFC fixture has printf
disabled: the comparison establishes shared router semantics and connections,
not equivalence of a complete Print-enabled platform address map.
`PrintCPUAllocationCompare.py` passes again for the unchanged CPU transport,
queue/count connectivity and recorded 6144x512, 13-bit occupancy geometry.

Seven focused CTests passed: Print binding, widget write binding, error slave,
address decode, read dispatch, write route and write dispatch. After extending
the connectivity assertions and explicit-catalog rejection coverage, the Print
binding and widget-write tests passed again. The Print fixtures cover ordered
and reversed two-host bindings plus a preceding TracerV stream: regions are
32/32/16 bytes at 0/32/64, all three request banks bind to their allocated lanes,
and both Print and count responses retain their flipped B/R contracts. Eleven
new composition failures (including failures after adapter construction) and
six malformed explicit binding catalogs preserve IR atomically.

Remaining: response tracker/arbitration composition for the selected Print
banks, full platform widget registration/address collateral and Print runtime
integration. The next smallest step is to connect these banks' B/R interfaces
to the native transaction trackers and response arbiters, then compare their
accepted-response and retirement behavior with the same immutable NastiRouter.
The prior harness's printf-disabled Rocket regressions passed; this iteration
runs only native compiler/unit comparisons and leaves manager verification to
the harness. The complete migration success matrix remains open.

### Iteration 68: selected Print control response composition

`mapPrintBridgeControlResponses` consumes the selected Print/count banks' B/R
interfaces through the native read tracker, burst-locking read arbiter,
round-robin write arbiter and write tracker. It checks the complete Print/CPU
binding catalog, stages all four passes on a clone, verifies the result and
moves only new modules into the original circuit. A late failure preserves
existing operations and annotation identities. The resulting boundary is
`GGControlWriteTrackerWrapper`; master request/response fields and tracker
match diagnostics remain explicit, while bank responses and tracker capacity
inputs are connected internally.

Use `--map-print-host-responses` after `--map-print-host-control` in the selected
`--compile-baseline --stop-after-print-host-binding --map-print-host-cpu-streams`
flow. Outputs are `post-print-control-responses.mlir`, `.sv` and `-all.json`.
The mutable evidence directory is `iteration68-print-control-responses/` under
the U250 generated-source tree. Its selected Rocket/SRAM compilation uses the
immutable `firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/`
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and iteration 67's selected
rf/printf annotations. One Print bank and one CPU count bank yield three
response sources including the error endpoint, with 64 slots and 12-bit tags
in each outstanding-transaction tracker.

`PrintControlResponsesCompare.py` compares emitted response retirement and ID
wiring with the exact immutable U250 `design/FireSim-generated.sv` recorded
above. Six Print/count/error R/B retirement predicates match: accepted final
R beats retire reads, and accepted B responses retire writes. Normal SFC MCR
responses are single-beat, so their optimized predicates specialize R.last to
one; the error endpoint retains R.last in both outputs. The native general
boundary preserves R.last for every bank. Actual emitted tracker enqueue,
capacity, ID, route, arbiter and retirement connections pass. Both outputs
retain 64 tag/free slots; SFC eliminates unused route-data storage, while the
native boundary exposes route diagnostics. Annotation classes, non-target
payloads and ordering are preserved. The previous request/count comparison
also passes all eight normalized SFC router predicates.

The Print binding test covers two ordered/reversed Print hosts and a preceding
TracerV stream: all three banks plus error bind to response arbitration, and
both trackers use four dequeue lanes with two-bit routes. Twelve new malformed
response compositions, including an unbound decoded region and failure after
both arbiters materialize, reject without changing the original IR. The focused native response tests
exercise collision/reset/retirement, stalls, round-robin selection and burst
locking across dynamic catalog sizes.

Remaining: assemble one coherent MMIO master bundle for the selected banks,
then integrate platform widget registration, address/driver collateral and
Print runtime execution. The immutable U250 fixture has printf disabled, so
this is a comparison of shared router semantics, not a complete Print-enabled
platform differential. Harness-owned manager gates were not started here.

### Iteration 69: selected Print control master assembly

`bindControlMaster` now accepts an explicit completed assembly stage. The
default full-platform entry still requires `GGFASEDBridgeBoundWrapper`; the
selected Print path explicitly requires `GGControlWriteTrackerWrapper` after
its complete Print/count request and response catalog has been bound. The pass
validates every U250 request/response field before creating FIRRTL operations.
It internalizes 32 scalar ports and the aggregate AR port, connects them to one
five-channel `ctrl` bundle and preserves the inner module body. Copied ports,
including host clock/reset and CPU AXI streams, retain their types and
directions. Copied annotation targets transfer to the new master wrapper;
internalized targets retain their inner module identity.

`--map-print-host-master` requires `--map-print-host-responses` and emits
`post-print-control-master.mlir`, `.sv` and `-all.json`. The mutable evidence
directory is `iteration69-print-control-master/` under the U250 generated-source
tree. The native candidate consumes the immutable compiler oracle's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` with the selected rf/printf
annotations from iteration 68: one Print bank, one CPU count bank and error.

`PrintControlMasterCompare.py` compares the emitted interface with `FPGATop`
in the exact immutable U250 `design/FireSim-generated.sv` recorded above. All
45 control pins match in width and direction, including 25-bit addresses,
12-bit IDs, 32-bit data and all sidebands. All 45 actual master-to-router
connections pass. The 214 copied flattened ports retain their interfaces;
CIRCT folds the constant CPU R.resp output to the same `2'h0` in both the
inner and outer emitted modules. Annotation classes, payloads, order and
recursive target transfer match. `control-master-comparison.json` records
these results and both exact artifact paths. The response comparison also
passes all six retirement predicates and tracker/arbiter connections; the
request comparison passes all eight SFC NastiRouter handshake predicates.

The compiler and both focused tests built. `goldengate-print-binding` and
`goldengate-control-master` pass, covering ordered/reversed two-Print-host
catalogs with a preceding TracerV stream, complete scalar/aggregate wiring,
copied CPU stream ports, unchanged inner IR, explicit-stage enforcement and
26 atomic master rejection cases across both assembly stages. The CLI rejects
the new flag without response composition before creating an output directory.

Remaining: integrate selected Print widget registration and address/driver
collateral into platform assembly, then exercise Print runtime execution.
The next smallest step is to emit the selected Print/count register-address
collateral from the same bound catalog used by MMIO dispatch. The immutable
U250 fixture has printf disabled, so shared control ABI/router matches do not
establish a Print-enabled platform differential. This iteration did not start
any harness-owned manager gates; the overall migration remains incomplete.

## Iteration 70: allocated selected Print/count driver collateral

The selected control-master boundary now emits
`print-bridge-allocated.const.h`. Its six Print addresses, widget numbers,
CPU stream indices, count addresses, DMA bases and queue capacities come from
the materialized FIRRTL widget/transport catalog. The emitter checks the live
instance hierarchy, six-word configuration ABI, register permissions,
read/write bindings and decoder regions against native register allocation.
It preserves the input IR and existing output annotations on success or failure.
Standard `GET_INCLUDES`, `GET_SUBSTRUCT_CHECKS`, `GET_BRIDGE_CONSTRUCTOR` and
`GET_MANAGED_STREAM_CONSTRUCTOR` guards support the existing FireSim driver.

The shared CPU header analysis now follows scalar Print payload connections
through hierarchy, as well as aggregate stream connects. Payload and occupancy
must resolve to the same queue instance, including when queues share a module
definition. An explicit selected-outgoing boundary validates the selected
master before the CPU write engine has been assembled; it rejects an existing
incoming engine or incoming/write ports. The default complete boundary still
requires and checks the empty incoming AXI engine. This fragment is allocation
collateral for the partial selected assembly, not a completed platform header.

Evidence lives in `iteration70-print-allocated-header/` under the same mutable
U250 generated-source tree. The compiler consumes the exact immutable compiler
oracle `firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` recorded above with
selected Rocket rf/printf annotations. The emitted Print addresses are
0, 4, 8, 12, 16 and 20; stream index/DMA base are zero and the count address is
32. `PrintAllocatedHeaderCompare.py` compares that fragment against emitted
`post-print-control-master.mlir` and the exact immutable U250
`design/FireSim-generated.sv` recorded above. SFC's live read-only 32-bit count
word, 6144-by-512-bit queue and 524288-byte DMA window match. Its absolute count
address is 568, versus selected candidate address 32: the recorded full widget
catalog differs from this selected Print/count catalog. That expected mismatch
is retained in `allocated-header-comparison.json`. The fixture has printf
disabled and no generated driver header; no Print runtime differential is claimed.

The native compiler and focused tests build. Print binding covers both two-host
orders, a real preceding TracerV queue (Print CPU indices 1 and 2 versus widget
indices 0 and 1), register metadata permutation and 44 atomic allocated-header
rejections, including constant/detached scalar payloads. The complete CPU header
test passes its 65 atomic rejection cases. The selected fragment compiles as
C++17 against the real FireSim driver headers under all four guards. Repeated
structured comparisons also pass the three CPU handshake predicates, eight
request predicates, six response retirement predicates, all 45 SFC control ABI
pins and annotation target transfer.

Remaining: register Print in the complete platform widget catalog and allocate
its MMIO/count/driver addresses there, then assemble the complete CPU AXI
boundary and exercise Print runtime execution. The next smallest step is to
extend that platform catalog with the selected Print register descriptors;
the observed 32-versus-568 count address difference makes catalog integration
the next allocation boundary to compare. No harness-owned manager gate was
started in this iteration. The overall migration remains incomplete.

### Iteration 71 — native Rocket catalog registration for Print banks

`deriveRocketControlMMIOCatalog` now provides the production Rocket allocator's
registration list. It derives every bank size from materialized FIRRTL MCR
ports/register registries, including the six sparse FASED fragments. The
baseline compiler uses this function and compares its later SimulationMaster,
TSI and BlockDev adapters against the named allocated descriptors. Supplied
queued Print hosts enter after the original bridges (including Clock), before
LoadMem and CPU streams. This follows FPGATop.scala's bridge registration and
PrintSynthesis.scala's appended BridgeIO annotations. Host constructor order,
not module traversal, controls equal-sized Print bank order. Duplicate, foreign,
aliased, incomplete or omitted materialized queued hosts fail atomically.

Evidence is in `iteration71-print-platform-catalog/` under the mutable U250
generated-source tree recorded above. A fresh native baseline compiler run
consumes the immutable compiler oracle
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and its `.anno.json`.
`PrintBridgeBindingTest` derives the catalog from actual Rocket bank modules,
imports two native queued Print hosts, then creates verified native decoder
operations for both host orders. The 11-bank baseline remains unchanged. With
two Print banks, their regions are 544/576 (32 bytes each), and the small-bank
tail shifts by 64 bytes: master 608, reset 624, count 632. The count bank at this
pre-binding fixture still contains only TracerV's word. Adding Print occupancy
words later changes its size and may change its sorted position; 632 is not a
completed Print-enabled platform driver address.

`PrintPlatformCatalogCompare.py` compares both native fixture decoders and the
fresh compiler's `post-fame-control-address-decode.mlir` with the exact immutable
U250 `design/FireSim-generated.sv` recorded above. All 11 baseline AW/AR regions
and FPGATop slave bindings match SFC. The expanded catalog's two additional
32-byte regions and shifted tail match the SFC allocator semantics; the fixture
has Print disabled, so no Print-enabled SFC/runtime parity is claimed. Results
are retained in `platform-catalog-comparison.json`. Native Print binding/header
checks pass, including 20 new atomic catalog rejections. The control decoder
suite passes 123462 address pairs and its registry/allocation rejection cases.
The fresh baseline compiler completes native simulator RTL and collateral
emission with empty stderr, including `.const.h`, `.const.vh` and XDC.

Remaining: attach the selected queued Print hosts and their occupancy words to
the complete platform CPU stream catalog, then bind their full-platform MMIO
requests/responses and emit headers from the resulting actual allocation. No
harness-owned manager gate was started. The overall migration is incomplete.
### Iteration 72: Rocket queue identity and appended Print DMA/count words

`deriveRocketCPUStreamPorts` resolves the active top's TracerV payload and
occupancy ports through FIRRTL wrapper connects to the same queue instance.
It validates the recorded stream identity, index zero, 6144x512 storage and
13-bit occupancy. It leaves the circuit and both result lists unchanged on
failure. The full `--compile-baseline` compiler now uses this derived pair for
CPU read and count allocation, replacing separate implicit default lists.
`mapPrintBridgeRocketCPUStreams` uses the same preflight and appends bound Print
hosts in constructor order through the existing transactional native builders.

Evidence is under mutable `iteration72-print-rocket-streams/`. The native
binding test imports the actual candidate `post-fame-tracerv-stream-queue.mlir`
hierarchy into the two-domain Print token fixture, forwards its queue outputs,
then binds the Print hosts and materializes one shared CPU read/count transport.
Both constructor orders preserve TracerV at index zero and append Print at
indices one/two with DMA bases 524288/1048576. The live count words have offsets
0/4/8; the native MCR adapter derives three words, requiring a 16-byte region.
Fourteen rejection cases cover missing identity/geometry metadata, detached or
multiply driven counts, and a count from another instance of the same queue
definition. Existing binding, decoder, header and response tests still pass.
The multi-stream transport interpreter passes 196241 SSA cases, including
global R-handshake counter updates under backpressure and unselected streams.

Fresh native compilation reads the immutable compiler oracle's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json` and emits
complete baseline RTL/collateral with empty stderr. `PrintRocketStreamsCompare.py`
compares the candidate against the immutable U250 fixture's
`design/FireSim-generated.sv`: all three CPU read handshake predicates, 19-bit
DMA window selector, queue capacity/width and 13-bit count forwarding match.
It also checks both expanded native allocation/count artifacts.
`PrintPlatformCatalogCompare.py` again matches all eleven baseline AW/AR regions
and slave bindings against that same golden artifact.

The SFC fixture disables Print. The expanded stream test is an IR attachment
boundary with explicit Print token inputs, not a runnable complete Print-enabled
platform. The pre-binding catalog test still has its original one-word count
bank; it must not be confused with the expanded three-word transport. Next,
carry this expanded count bank into the full Rocket MMIO catalog and dispatch,
then derive full-platform Print driver addresses. Manager gates remain owned by
the verification harness; the SFC UART-bearing workload baseline is still pending.

### Iteration 73: expanded CPU occupancy bank in the complete Rocket MMIO map

`allocateRocketControlMMIORegions` composes the materialized platform register
catalog with the shared TracerV/Print DMA/count catalog before creating the
decoder. The full baseline compiler now calls this native API. It requires
one read-only occupancy word per DMA index, checks names and byte offsets
independently of metadata row order, and verifies the supplied Print constructor
order against the actually instantiated hosts. It derives typed MCR word counts
and applies `Widget.scala`'s power-of-two sizing, decreasing-size sort and
contiguous allocation. Failures leave the IR and both result lists unchanged.

Evidence is under mutable `iteration73-print-rocket-mmio/`. The binding test
imports the fresh native Rocket queue hierarchy and actual register banks into
the two-domain queued Print fixture. Historical bank dependencies use a separate
symbol namespace so their old CPU wrapper cannot alias the live expanded CPU
wrapper. Both Print constructor orders produce thirteen regions: Print bases
544/576 remain stable; the three-word CPU bank requires sixteen bytes and sorts
at 624 before ResetPulse at 640. The old pre-binding one-word count catalog's
CPU base 632 is therefore not the final expanded allocation. Metadata row
permutations preserve the map. Forty atomic rejection cases cover queue identity,
missing/duplicate DMA indices and occupancy words, bad register schema,
insufficient address space, detached Print bindings and changed host order.
Existing selected Print bindings, headers, requests/responses and generic
decoder tests continue to pass, including 123462 interpreted address pairs.

A fresh direct native `--compile-baseline` reads the immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json`. It emits
complete RTL/collateral with empty stderr. `PrintRocketMMIOCompare.py` compares
the candidate decoder with the immutable U250 fixture's
`design/FireSim-generated.sv`: all eleven baseline AW/AR ranges and widget slave
bindings match. It then verifies both expanded native decoders and their actual
three-word count banks. `PrintRocketStreamsCompare.py` retains the matching SFC
TracerV handshake/storage/count geometry; its reader query now selects the
active shared transport explicitly because the imported bank dependencies also
contain isolated historical readers. `PrintPlatformCatalogCompare.py` continues
to validate the earlier pre-binding allocation separately.

The immutable reference disables Print. This expanded test establishes the
actual-bank allocation and native decoder boundary; it does not attach every
platform request/response port or provide a runnable Print-enabled platform.
Next, bind the full thirteen-bank dispatcher to these allocated ports, then
derive Print driver addresses from that bound map. Manager verification remains
owned by the harness. The overall migration and SFC UART differential remain
incomplete.

### Iteration 74: Rocket request binding includes instantiated Print banks

`bindRocketControlWidgetWrites` extends the production compiler's seven early
MMIO request bindings with every instantiated Print constructor. It resolves
the sorted slave indices by widget identity, requires the complete ordered
Print host registry, and follows whole AXI bundle forwarding through the active
wrapper hierarchy to the matching host's control operand. Equal AXI types alone
do not identify a bank: swapped same-type Print ports, detached ports, fanout
and cycles must fail before any IR or annotation mutation. The existing FIRRTL
AW/W binding operations and subsequent AR dispatcher now consume this expanded
catalog. Master, FASED, TSI and BlockDev still use their later binding steps.

Evidence is under mutable `iteration74-print-rocket-requests/`. The native
fixture preserves the five live Rocket bridge controls while binding two
queued Print hosts and their shared TracerV/Print DMA/count engine. It imports
the actual LoadMem AXI adapter with its register side exposed. Both Print
constructor orders bind nine early request banks through the complete thirteen
regions. Native checks verify all 180 AW/W scalar connections per order,
including shared Nasti metadata and independent readiness, and the matching AR
bank identities. Print slaves are 8/9; the expanded CPU bank is slave 11 at 624;
ResetPulse moves to slave 12 at 640. Fifty-four atomic rejection cases cover
the combined Rocket/Print stream, allocation and request contracts, including
swapped same-type Print control ports. The generic widget test passes 420
scalar bindings and 30 rejections; the AR interpreter passes 248056 independent
backpressure/route cases across up to 63 regions, including thirteen regions.

A fresh direct native compile reads the immutable compiler oracle's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json`, emitting
baseline RTL/collateral with empty stderr. `PrintRocketRequestsCompare.py`
compares the candidate's seven baseline AW/W/AR bank identities with the
immutable U250 fixture's `design/FireSim-generated.sv`: all match. It then
checks both expanded native request catalogs against their actual allocation.
The MMIO, stream and pre-binding catalog comparisons also retain their SFC
matches. The final native register/queue input boundaries match those used by
the fixture (recorded in `input-boundary-comparison.json`).

The immutable SFC reference disables Print. This milestone establishes early
request attachment to live Print/CPU/original bridge storage, with an explicit
LoadMem register-side boundary and four later request/response banks exposed.
The complete Print-enabled CLI path, full response assembly and full-platform
Print driver addresses remain unfinished. Next, compose the later bank and
response bindings with this thirteen-region native fixture, then carry the
bound map into Print headers and the complete CLI path. Manager verification
remains owned by the harness; the SFC UART-bearing differential is pending.

### Iteration 75: Rocket/Print B/R response composition

`mapPrintBridgeRocketControlResponses` composes the existing native FIRRTL
read tracker, read arbiter, write arbiter and write tracker after the expanded
Rocket request boundary. It consumes the seven early Rocket banks and both
instantiated Print banks. The thirteen normal slaves plus the error source
produce fourteen arbitration inputs and retirement ports; both trackers use
64 slots, 12-bit response IDs and four-bit routes. The Scala oracle is
`junctions/nasti.scala`'s `NastiRouter`: accepted final R beats retire AR
transactions; accepted B responses retire AW transactions, using response IDs.

Before creating any operations, the compiler re-derives the thirteen-region
allocation from live register/DMA registries, requires identical AW/W and AR
catalogs, and resolves each bank's actual AR SSA connection to its allocated
slave. Matching stale metadata cannot silently exchange the two Print banks.
All four response passes run on a staged circuit; a later collision preserves
all original IR and annotations. The selected Print-only composition shares
this implementation and retains its existing behavior.

Evidence is under mutable `iteration75-print-rocket-responses/`. The native
fixture imports the actual Rocket hierarchy, banks and two queued Print hosts,
and composes both constructor orders. It verifies every connected B/R payload,
ID, ready/valid pair, final-beat retirement predicate, error response and all
four later-bank scalar boundaries. Raw annotation payloads and ordering are
preserved. The combined stream/allocation/request/response test passes 76
atomic rejections, including equal stale Print catalogs with swapped ports
and a failure after both response arbiters have been staged.
The shared arbiter interpreters pass 114833 read and 195366 write cases.
Tracker interpreters pass 14616 read and 4989 write collision/retirement/reset
cycles across up to 63 regions, including the expanded thirteen-region case.

A fresh direct native `--compile-baseline` reads the immutable compiler
fixture's `firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json`,
and emits complete baseline RTL/collateral with empty stderr. Its register
and TracerV queue input artifacts match the fixture's inputs and iteration 74
(`input-boundary-comparison.json`). `PrintRocketResponsesCompare.py` reads
actual FIRRTL SSA wiring in both response artifacts and compares the seven
shared banks plus error with the immutable U250 fixture's
`design/FireSim-generated.sv`: response identities, response IDs/data,
ready/valid wiring and retirement predicates match. The SFC normal banks
specialize R.last to one; the native test preserves the unspecialized
ready-and-valid-and-last predicate. `rocket-responses-comparison.json` records
the baseline/expanded slave mapping. The request, MMIO, TracerV stream and
pre-binding catalog comparisons retain their SFC matches.

The immutable SFC RTL disables Print, so it does not establish full
Print-enabled equivalence. Master, FASED, TSI and BlockDev request/response
ports remain exposed; LoadMem's register side also remains an explicit
boundary. The full Print-enabled CLI path and platform driver addresses are
unfinished. Next, attach and bind the actual SimulationMaster bank at expanded
slave 10, then compare its complete request/response/register path with SFC.
Manager verification remains owned by the harness; the SFC UART-bearing
baseline is pending and the overall migration remains incomplete.

### Iteration 76: expanded Rocket/Print SimulationMaster attachment

`mapPrintBridgeRocketSimulationMaster` composes the native Master.scala bank,
three-word MCRFile adapter, and AW/W/AR/B/R binding after the expanded response
tracker boundary. It uses the live thirteen-region allocation instead of the
old eleven-bank indices. SimulationMaster moves from slave 8/base 544 to slave
10/base 608; its three register addresses become 608, 612, and 616. The bank
retains its five registers, host clock/reset, and Widget.scala's default
ReadWrite permissions even for the genROReg/genWOReg-named helpers.

The compiler validates the complete live allocation, instantiated Print
constructor order, response catalog, and Master register identities/offsets/
permissions before staging all three operations. A collision after attachment
or adapter creation leaves the original circuit unchanged. Inspection of the
first fixture failure corrected the catalog owner: it is retained on
`GGControlReadArbiterWrapper`, not the later write-tracker wrapper.

Evidence is under mutable `iteration76-print-rocket-master/`. The native
Rocket/two-Print fixture passes both constructor orders and 94 atomic
rejections (18 new Master cases). It checks all 32 scalar requests/responses,
aggregate AR, host clock/reset, consumption of the Master boundary, copied
port types/directions, unchanged decoder/register catalogs, and retained
annotation payload/order. The SimulationMaster behavior and generic control
binding tests also pass. A fresh final native `--compile-baseline` reads the
immutable compiler fixture's `firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
and `.anno.json`, emits baseline RTL/collateral with empty stderr, and produces
bank/queue input artifacts identical to the fixture inputs by SHA-256.

`PrintRocketMasterCompare.py` compares actual native SSA wiring with the
immutable U250 fixture's `design/FireSim-generated.sv`: all 20 surviving SFC
Master control pins, host clock/reset, five register widths/reset values, and
three read-word identities match after normalizing the allocation shift. The
complete native bank operations also match the fresh baseline bank. Both host
orders pass; `rocket-master-comparison.json` records the exact artifacts and
addresses. The response, request, MMIO, TracerV stream, and pre-binding catalog
comparisons retain their SFC matches.

The immutable SFC fixture disables Print. This does not establish full
Print-enabled platform equivalence or complete driver assembly. TSI, BlockDev,
and FASED request/response boundaries remain exposed, as does LoadMem's
register side. Next, compose the native TSI queue/MMIO attachment at slave 3
with this expanded boundary and compare its register/control path with SFC.
The full Print-enabled CLI path and driver addresses remain pending. Manager
verification remains owned by the harness; the SFC UART-bearing baseline and
overall migration remain incomplete.


### Iteration 77: native TSI attachment after expanded Rocket/Print Master

`mapPrintBridgeRocketTSI` composes the existing native TSI token scheduler,
its two sixteen-word queues, attachment of the materialized nine-word MMIO
bank, MCRFile adapter, and allocated control binding. Its input is the
completed `GGSimulationMasterBoundWrapper`; its output is
`GGTSIBridgeBoundWrapper`. TSI remains slave 3 at byte 320 with a 64-byte
region in both constructor orders. Print does not shift this earlier bank.
The compiler checks register identities/offsets/permissions and the complete
live 13-bank allocation before staging the operations. Missing channel or
bank identities and collisions at late adapter/binding stages reject without
changing the caller's circuit. Only newly generated modules and the completed
annotation targets are committed; existing Rocket/Print operations remain.

The native fixture forwards the five actual TSI ports and their constructor,
clock, channel payload and forward handshake descriptors from the ingested
Rocket queue boundary. It connects them to the imported live Rocket instance.
The first runs identified two fixture faults: absent StringAttr members must
be checked before comparison, and the manually composed LoadMem wrapper
must transfer copied top-port module identities along with circuit prefixes.
The fixture now also requires retention of all five completed TSI channel
annotations with both source and sink endpoints. TSI consumes the external
ports while retaining the completed channel metadata.

Evidence is under mutable `iteration77-print-rocket-tsi/`. The corrected native
Rocket/two-Print fixture passes both host orders and 114 atomic rejections,
including 20 new TSI cases. These cover stale region base/slot, invalid register
permission, missing register/region names, missing reset channel, late adapter
and binding symbol collisions, wrong active top and repeated attachment.
The existing TSI token, word-queue and MMIO behavior tests all pass. The fresh
native `--compile-baseline` reads the immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir` and `.anno.json` and emits
RTL/collateral with empty stderr. Its decoder and TracerV queue fixture inputs
have the same SHA-256 values as iteration 76.

`PrintRocketTSICompare.py` compares both expanded artifacts with the immutable
U250 reference's `design/FireSim-generated.sv`. The nine read-word identities,
ten scheduler/MMIO register widths, slave-3 region, host clock/reset and all
20 surviving SFC control pins match. The complete native MMIO bank, scheduler,
queue and MCRFile operations remain identical to the fresh baseline. Actual
SSA nets contain 32 scalar control connections plus the aggregate AR channel,
and connect the bank to both independent queues and scheduler. Results are
recorded in `rocket-tsi-comparison.json`. The Master, response, request, MMIO,
TracerV stream and pre-binding platform catalog comparisons also pass.

The immutable SFC fixture disables Print: this establishes the shared TSI
boundary, not full Print-enabled platform or driver equivalence. BlockDev and
FASED controls and LoadMem's register-side attachment remain separate work;
full Print-enabled CLI assembly remains pending. Next, forward the actual
BlockDev channels and compose its native timing/queue/MMIO path at slave 0
through this expanded boundary, comparing its register/control path with SFC.
Manager verification remains harness-owned. The SFC UART-bearing baseline,
FPGA execution and overall compiler migration remain incomplete.

## Iteration 78: expanded Rocket/Print BlockDev boundary

`mapPrintBridgeRocketBlockDev` composes the existing native BlockDev token,
request/data/read-response/write-ack queues, 26-word bank, allocated control
adapter, latency pipes, and write-priority response scheduler after the live
expanded Print/TSI boundary. It validates every bank word's identity, offset,
and permissions and re-derives the whole thirteen-bank allocation before
staging operations on a cloned CIRCT circuit. The actual allocated bank is
attached without replacing its operation or registry. Missing channels,
mismatched clocks, stale allocation, malformed metadata, and late adapter,
binding or scheduler collisions reject without changing the input circuit.

The fixture forwards the nine actual Rocket BlockDev ports, nested forward
valid/ready descriptors and original constructor metadata, in addition to the
five TSI channels. Both Print constructor orders retain all nine completed
BlockDev channel annotations and one one-tracker constructor. All four queues
have independent instances; their shared reset is the engine's qualified
host-or-target reset. Timing uses the engine's target cycle counter and MMIO
latencies. The control adapter consumes slave 0's AW/W/AR/B/R boundaries.
The remaining latency/reset/tFire diagnostics are preserved as in the native
pipeline rather than treated as external target channels.

`PrintRocketBlockDevCompare.py` compares both emitted boundaries against the
immutable U250 SFC `design/FireSim-generated.sv`: BlockDevBridgeModule's 26
register identities and permissions (two geometry words are write-only), 31
register/state widths, 24 read slots, four queue depths/payload widths
(10/66, 32/65, 32/65, 4/1), qualified queue reset, and 20 surviving SFC control
pins. The actual native bank, token engine, queue definitions, adapter, latency
pipes and response scheduler must also match the fresh native Rocket candidate
operations. Actual queue/MMIO/scheduler/latency SSA connections and slave 0's
0--127 byte AW/AR region are checked, with no tail-bank allocation normalization
needed for BlockDev.

Evidence lives under the mutable generated-source directory's
`iteration78-print-rocket-blockdev/`: fresh native compilation, expanded fixture,
focused BlockDev token/queue/MMIO/control/timing tests, and structured comparisons.
The fixture passes both constructor orders and 138 atomic rejection cases;
all ten focused BlockDev tests and all eight Rocket/Print structured comparison
scripts pass. FireSim manager gates for this checkpoint remain harness-owned.
The immutable SFC fixtures are read-only. Its Rocket configuration disables
Print: this establishes a shared BlockDev boundary, not complete Print-enabled
platform/driver or FPGA execution equivalence. FASED, LoadMem register-side
assembly, the UART-bearing SFC runtime baseline and hardware validation remain
pending. Next: forward actual Rocket FASED channels and attach its native MMIO
and timing path at allocated slave 1 in the expanded Print platform.

## Iteration 79: FASED token and ingress composition after Rocket/Print BlockDev

`mapPrintBridgeRocketFASEDIngress` composes the native FASED token engine,
host outstanding counters, AW ingress queue, W ingress queue and AR ingress
queue after `GGBlockDevResponseSchedulerWrapper`. It validates the complete
live thirteen-bank allocation and ordered Print host identities, stages all
five passes on a cloned CIRCT circuit, verifies it, then commits only new
modules, completed annotation endpoints and the new circuit identity.
Existing bank operations and their registries remain in place. The FASED bank
remains slave 1 at byte 128 with size 128; this step does not attach its MMIO.

The expanded fixture forwards all eleven actual FASED channel ports and their
constructor, clock and nested valid/ready descriptors from the native Rocket
handoff. Channel checks derive identities from the actual constructor mapping
rather than assuming a global-name prefix. Both endpoints are retained after
host attachment; consumed target token ports and intermediate enqueue/readiness
ports disappear from the active top. Three independent queue instances retain
host-clock and qualified ingress-reset wiring. Host outstanding counters use
host reset only, so target stalls/reset do not discard host transactions.

`PrintRocketFASEDIngressCompare.py` compares emitted operations and SSA nets
with the immutable U250 `design/FireSim-generated.sv` and the fresh native
`post-fame-fased-ingress-ar.mlir`. It checks SFC's full target-fire versus
exclude-ingress-ready predicate, qualified ingress/egress resets, host counter
width/max/reset, queue depths, surviving payload fields and enqueue predicates
excluding each queue's own ready signal. Native address queues retain 69 bits
and the W queue 78 bits. The optimized SFC queues retain 64/73/64 bits because
unused user/region (and W ID) fields are pruned; this is recorded explicitly.
It also evaluates all 256 token/readiness/reset input combinations per order
and checks actual wrapper clock/reset/enqueue/dequeue connections.

Evidence is under mutable `iteration79-print-rocket-fased-ingress/`, including
fresh CIRCT compilation of the immutable compiler oracle's `.sfc.fir` and
`.anno.json`, focused FASED behavior tests and expanded binding outputs.
The fixture passes both constructor orders and 156 atomic rejection cases,
including eighteen new FASED ingress cases. All five focused FASED behavior
tests, the basic Print binding test and all nine structured comparisons pass.
Fresh native compilation has empty stderr. The decoder and TracerV queue inputs
match iteration 78 by SHA-256. Manager compile/metasim/bitstream gates remain
exclusively harness-owned.

This is a partial FASED ingress boundary. Egress readiness, host transaction
signals and raw queue dequeues remain external; ingress credits/order/issue,
egress scheduling, timing, MMIO and host-memory binding follow. The reference
disables Print, so the comparison establishes shared FASED semantics without
claiming full Print-enabled CLI/driver or hardware equivalence. Next smallest
step: compose native ingress credit/order/issue/deadlock passes through this
expanded boundary, compare their accepted host issue behavior with SFC, then
advance to egress/timing and slave-1 MMIO attachment. The UART-bearing SFC
runtime baseline and overall migration remain incomplete.


## Iteration 80: FASED host issue and deadlock after expanded Rocket/Print ingress

`mapPrintBridgeRocketFASEDIssue` composes native ingress credits, transaction
order, host issue and deadlock passes after the expanded ingress boundary.
It checks the ordered Print host catalog and complete live thirteen-bank MMIO
allocation before staging the batch on a cloned CIRCT circuit. Commit preserves
existing ingress module operation identities while transferring the assertion
context ports and bodies. The six FASED register fragments and control decoder
remain intact. Helper collisions, stale allocation, unsupported credit profile,
wrong circuit identity and repeated application reject without changing IR.

The issue wrapper consumes the raw AW/W/AR dequeues, transaction-order dequeue
and host outstanding transaction inputs. It exposes host request and response
bundles. Credits retain independent saturating AW/W counters and ordered-mode
complete-write detection. The two ten-entry order queues preserve read-before-
write ordering on simultaneous enqueue. Host issue implements relaxed credit
gates and conservative read/write ordering; AW acceptance takes priority over
last-W acceptance when updating the write-data state. Deadlock assertions
observe raw enqueue valid/ready under qualified ingress reset, with their policy
context threaded through the existing AW/W/AR ingress hierarchy.

`PrintRocketFASEDIssueCompare.py` uses the immutable U250 build reference's
`design/FireSim-generated.sv`, specifically `IngressModule` and `DualQueue`.
It checks the oracle's credit, issue and assertion equations, evaluates native
SSA for 3,872 credit, 32,768 issue and 512 deadlock combinations per Print order,
and checks actual helper clock/reset/handshake connections. Helper definitions
must match the fresh native `post-fame-fased-ingress-deadlock.mlir`. Constructor
and channel identities, all six FASED register fragments and the decoder must
survive composition. The fixture's `--fased-issue-boundary` mode can replay a
saved expanded ingress artifact to isolate the batch and its nine rejections.

Evidence is in mutable `iteration80-print-rocket-fased-issue/`. Fresh native
compilation of the immutable compiler `.sfc.fir` and `.anno.json` succeeds with
empty stderr. The focused Print binding and four FASED ingress tests pass. The
expanded fixture passes both constructor orders and 174 atomic rejection cases,
including eighteen new issue/deadlock cases; isolated replay passes nine cases
and emits exactly the same first-order IR as full assembly. All ten structured
Rocket/Print comparisons pass, including the credit/issue/deadlock cases and
actual SSA wiring in both constructor orders. This is still a
partial FASED boundary: host response bundles, egress readiness, timing policy
and MMIO attachment remain external. The recorded SFC configuration disables
Print; shared FASED comparisons do not establish a complete Print-enabled
production CLI or driver flow. Manager verification remains harness-owned.
Next: compose the native read buffer/read scheduler and write egress/response
releaser through this expanded boundary, then timing and slave-1 MMIO. The
UART-bearing SFC runtime baseline and overall compiler migration remain pending.

## Iteration 81: per-ID read buffering after expanded Rocket/Print host issue

`mapPrintBridgeRocketFASEDReadBuffer` composes the native read buffer after
`GGFASEDIngressIssueWrapper`. It shares the issue stage's live Rocket/Print
allocation validation, stages on a cloned circuit, verifies the result, and
commits only the new helper/wrapper plus retargeted circuit annotations.
Existing module operations retain their identities. It consumes the flat host
response bundle into explicit read payload and write handshake ports, exposes
the raw read dequeue and address, and drives the buffer with hostClock and the
qualified egress reset. Host R ready remains unconditional in this non-ROB
profile; internal queue fullness still gates RAM writes. Reset clears pointer,
full and dequeue-valid state, leaving the packed RAM and delayed read ID unreset.

The expanded fixture invokes this stage after host issue in both constructor
orders. Saved iteration-80 issue boundaries can also be replayed independently
with `--fased-read-buffer-boundary` and
`--fased-read-buffer-boundary-reverse`. Each replay checks nine atomic
rejections: helper/wrapper collisions, stale or incomplete allocation, unsupported
ID-reuse/data width, wrong active top, missing annotations and repeated use.
Earlier module identities, the live FASED bank and MMIO allocation must survive.

`PrintRocketFASEDReadBufferCompare.py` compares the immutable U250 reference
`design/FireSim-generated.sv` (`ReadEgress` and `MultiQueue`) with the emitted
expanded read-buffer boundary. It resolves SFC's lowered vector selectors,
checks current versus delayed dequeue ID, synchronous read/prefetch equations,
16-by-8 queue geometry, data/last storage, all 49 reset registers and the unreset
read ID. Native storage is one packed 128-by-65 memory; SFC uses separate data
and last memories. Fourteen actual wrapper connections, all 227 prior module
definitions and the complete annotation archive must match in both Print
orders. The queue definition must also match fresh native compilation's
`post-fame-fased-read-buffer.mlir` from the immutable `.sfc.fir`/`.anno.json`.

Evidence is in mutable `iteration81-print-rocket-fased-read-buffer/`. Both
expanded replays and the structured SFC comparison pass with empty stderr.
The allocator refactor's issue replay passes nine rejection cases and emits
unchanged iteration-80 issue IR. Fresh native compiler ingestion and baseline
assembly pass with empty stderr. Focused Print binding and read-buffer tests
pass, including 40,000 native buffer state/edge samples and 18 lower-level
atomic rejection cases.

This remains a partial expanded FASED assembly, exercised through the compiler
library and boundary fixture. Print-enabled production CLI/driver integration,
request scheduling, write egress, response release, timing policy, slave-1 MMIO
and host-memory attachment remain incomplete. The SFC reference disables Print;
these shared semantics do not establish full Print-enabled runtime equivalence.
Next smallest step: compose the native read scheduler to consume the raw buffer
address/dequeue, compare its request/response predicates with SFC ReadEgress,
then compose write egress and response release. Manager gates remain
harness-owned; the UART-bearing SFC runtime baseline and overall port are pending.


### Expanded Print/Rocket FASED read request scheduling (iteration 82)

`mapPrintBridgeRocketFASEDReadScheduler` consumes the completed expanded read
buffer boundary. It revalidates the live Print registry and all allocated MMIO
regions before staging native `addFASEDReadScheduler` on a clone. After MLIR
verification, it publishes only the new helper/wrapper and retargeted circuit
annotations. All preceding module operations and definitions remain intact.
The wrapper consumes read address/dequeue and aggregate egress readiness,
connects read readiness to the scheduler's response hValid, and leaves explicit
read request/response and write readiness ports for subsequent timing lowering.

This implements the recorded no-ROB ReadEgress scheduling branch through CIRCT
FIRRTL operations: request capture and beat retirement use qualified target
fire, a new request wins over an accepted final beat, and only request-valid
state resets. The request ID captures even when reset and start coincide.
Dequeue-ready and retirement deliberately do not include buffer-valid; legal
cycles depend on token readiness. Response data/last and the stored response
ID are forwarded, while a newly arriving request selects the prefetch address.
The scheduler uses hostClock and the token engine's qualified egress reset.

The complete expanded fixture invokes this stage in both Print constructor
orders. Saved iteration-81 read-buffer boundaries can also be replayed using
`--fased-read-scheduler-boundary` and
`--fased-read-scheduler-boundary-reverse`. Each replay checks nine atomic
rejections covering collisions, malformed allocation, unsupported profile,
wrong top, missing annotation archive and repeated composition.

`PrintRocketFASEDReadSchedulerCompare.py` compares immutable U250
`design/FireSim-generated.sv` modules `ReadEgress` and `FASEDMemoryTimingModel`
with the emitted expanded scheduler. It pins SFC combinational equations,
reset/capture/retirement priority and host connections, then interprets actual
native SSA for all 32,768 flag and AXI ID combinations in each constructor
order. It also checks the qualified wrapper wiring, every prior module
and the entire annotation archive after the expected target rename. The
scheduler definition must match fresh native compilation's
`post-fame-fased-read-scheduler.mlir` from the immutable `.sfc.fir`/`.anno.json`.

Evidence is in mutable `iteration82-print-rocket-fased-read-scheduler/`.
Both expanded replays and the structured SFC comparison pass with empty stderr.
The comparison preserves all 229 preceding modules and the complete annotation
archive in each order, including nine qualified wrapper connections. It covers
1,024 simultaneous starts/retirements, 4,096 reset/start coincidences and 1,024
retirements without buffer-valid per order. Fresh native compiler ingestion and
baseline assembly pass with empty stderr.
Focused Print binding and read-scheduler CTests pass, including 32,768 native
scheduler edge cases, 32 wrapper mapping cases, eight annotation transfers,
and 20 lower-level atomic rejections.

The expanded Print-enabled assembly remains partial and is exercised through
the compiler library and boundary fixture. Write egress, response release,
timing policy, slave-1 MMIO and host-memory attachment remain to compose, as
well as Print-enabled production CLI/driver integration. The golden reference
has Print disabled and the UART-bearing SFC runtime baseline remains pending.
Next smallest step: compose native write egress against SFC WriteEgress,
then connect read/write release to the timing model. Manager gates remain
owned by the verification harness; overall port completion is not established.

### Expanded Print/Rocket FASED write acknowledgment scheduling (iteration 83)

`mapPrintBridgeRocketFASEDWriteEgress` consumes the expanded read-scheduler
boundary. It revalidates the live Print registry and allocated MMIO regions,
stages native `addFASEDWriteEgress` on a clone, verifies the resulting CIRCT
FIRRTL IR, then publishes only the new helper/wrapper and retargeted circuit
annotations. All preceding module operations remain intact. The wrapper
consumes flat host B response acceptance and write readiness, connects its
response hValid to token readiness, and preserves read scheduling and host R.
Write request/response ports await timing-model response release.

The recorded idReuse=1 profile has sixteen one-bit wrapping acknowledgment
counters. Host B always accepts and retains only its ID. `haveAck` samples old
counter state on a request start or retry, including retries without target
fire. Retry selects the stored ID even when a new request starts simultaneously.
Same-ID enqueue and retirement cancel; distinct IDs update independently.
A new request wins over retirement. Qualified egress reset clears valid,
haveAck and counters; the unreset request ID still captures during reset/start.

The complete expanded fixture calls this stage in both Print constructor
orders. Saved iteration-82 read-scheduler boundaries can also be replayed with
`--fased-write-egress-boundary` and
`--fased-write-egress-boundary-reverse`. Each replay checks nine atomic
rejections: helper/wrapper collisions, stale/malformed allocation, unsupported
profile, wrong top, missing annotation archive and repeated composition.

`PrintRocketFASEDWriteEgressCompare.py` uses immutable U250
`design/FireSim-generated.sv` modules `WriteEgress` and `FASEDMemoryTimingModel`.
It pins the SFC equations, all counter selectors and updates, sequential
priority and host wiring, then interprets actual native SSA for 65,536
flag/ID/counter-mask cases per constructor order. Complementary masks exercise
both values of every counter. The comparison checks qualified wrapper wiring,
all preceding module definitions and the complete retargeted annotation
archive. The helper must match fresh native compilation's
`post-fame-fased-write-egress.mlir` from immutable `.sfc.fir`/`.anno.json`.

Evidence is in mutable `iteration83-print-rocket-fased-write-egress/`.
Both expanded replays and the structured SFC comparison pass with empty stderr.
The comparison retains all 231 preceding modules and the complete annotation
archive in each order, checking thirteen actual wrapper connections. Each order
covers 128 same-ID cancellations, 8,192 host-only retries, 2,048 simultaneous
starts/retirements, 16,320 counter wraps, 896 old-counter samples on concurrent
enqueue and 8,192 reset/start coincidences. Fresh native compiler ingestion,
baseline host assembly and RTL emission pass with empty stderr. Both focused
CTest targets pass.

Run the focused Print binding and write-egress CTests, replay both expanded
boundaries, and run the comparison with the immutable SV path and the
iteration-82 evidence directory. The existing native write-egress unit test
also interprets 42,768 transitions, including 10,000 continuous state steps,
64 wrapper mapping cases, five annotation transfers and 21 atomic rejections.

The expanded Print-enabled assembly remains partial and is exercised through
the compiler library and boundary fixture. Timing-model response release,
timing policy, slave-1 MMIO, host-memory attachment and Print-enabled
production CLI/driver integration remain to compose. The golden reference
has Print disabled and the UART-bearing SFC runtime baseline remains pending.
Next smallest step: compose native `FASEDResponseReleaser` to consume both
egress request/response boundaries and compare accepted-last-beat retirement
and same-cycle replacement with SFC AXI4Releaser. Manager gates remain owned
by the verification harness; this does not establish overall port completion.

### Expanded Print/Rocket FASED response release (iteration 84)

`mapPrintBridgeRocketFASEDResponseReleaser` closes the expanded write-egress
boundary with native `addFASEDResponseReleaser`. It validates the live Print
registry and MMIO allocation, stages the transform on a clone and verifies
CIRCT FIRRTL before publishing its helper, wrapper and retargeted annotations.
All earlier module operations, Print banks and FASED latency registers remain
intact. The wrapper consumes timing R/B and both egress request/response
boundaries; host AXI and timing AW/W/AR remain connected. Completion metadata
enters through `fased_next_read` and `fased_next_write`.

AXI4Releaser uses two one-entry pipe queues for response occupancy. Empty
queues do not bypass newly accepted metadata into valid. Reads retire only
on an accepted last beat; writes retire on response acceptance. Retirement and
replacement in the same cycle leave occupancy set. Egress requests report
combinational completion acceptance, including during a token stall. Response
payloads and ready forward directly even when invalid. Occupancy and reset
advance only on targetFire, matching the SFC timing model's gated clock.

The complete expanded fixture calls this composition in both Print constructor
orders. Replay saved iteration-83 boundaries with
`--fased-response-releaser-boundary` and
`--fased-response-releaser-boundary-reverse`. Each replay checks nine atomic
rejections: helper/wrapper collisions, stale/malformed allocation, unsupported
ID/data widths, wrong top, missing annotation archive and repeated composition.

`PrintRocketFASEDResponseReleaserCompare.py` compares immutable U250
`design/FireSim-generated.sv` modules `AXI4Releaser`, its two pipe queues,
`LatencyPipe` and `FASEDMemoryTimingModel`. It pins SFC queue equations and
sequential priority, read-last retirement, response forwarding and gated clock
wiring, then interprets actual native SSA in both expanded constructor orders.
It checks 131,072 flag/read-ID/write-ID combinations per order, all earlier
module definitions, the complete retargeted annotation archive and 23 wrapper
connections. The helper must match fresh native compilation's
`post-fame-fased-response-releaser.mlir` from immutable `.sfc.fir`/`.anno.json`.
The shared SSA evaluator now supports the emitted FIRRTL XOR operation.

Evidence is in mutable `iteration84-print-rocket-fased-response-releaser/`.
Both expanded replays, the structured comparison and focused CTests pass.
All 233 preceding modules and the full annotation archive are retained in each
order. Coverage per order includes 2,048 read replacements, 16,384 non-last
beats, 24,576 blocked read requests, 24,576 occupied stalled resets, 37,376
combinational acceptances without fire and 32,768 empty-queue pushes without
flow-through. Fresh native ingestion, host assembly and RTL emission pass;
compiler, replay and comparison stderr logs are empty.
Run the focused Print binding and response-releaser CTests, both expanded
boundary replays, then the comparison with the immutable SV path and the
iteration-83 evidence directory. The native response-releaser unit test
interprets 18,192 transitions, including 10,000 continuous state steps,
64 wrapper mapping cases, seven target transfers and 22 atomic rejections.

The expanded Print-enabled assembly remains partial and is exercised through
the compiler library and boundary fixture. Timing policy, slave-1 MMIO,
host-memory attachment and Print-enabled production CLI/driver integration
remain to compose. The golden reference disables Print and the UART-bearing
SFC runtime baseline remains pending. Next smallest step: compose native
`FASEDTimingCycle` and compare target-qualified reset/advance, 64-bit cycle
wrap and runtime-latency release offsets against SFC LatencyPipe. Manager gates
remain owned by the verification harness; overall port completion is not
established.

### Expanded Print/Rocket FASED timing cycle (iteration 85)

`mapPrintBridgeRocketFASEDTimingCycle` composes native `addFASEDTimingCycle`
after the expanded response releaser. It validates the live Print registry and
MMIO allocation, verifies a staged CIRCT FIRRTL circuit, then publishes only
the new helper/wrapper and retargeted annotation archive. Earlier module
operations, response boundaries, Print banks and latency registers remain
intact. The wrapper adds the 64-bit model cycle, two 32-bit runtime latency
inputs and two 64-bit release-cycle outputs.

TimingModel.scala increments its model cycle on the gated model clock.
The native register advances only on targetFire; model reset is sampled on
the same enabled edge, so a reset during a token stall retains the cycle.
LatencyBandwidthPipe.scala computes releaseCycle as zero-extended runtime
latency plus the current cycle, less the one-cycle AXI4Releaser delay.
Both counter and deadlines wrap modulo 2^64, including latency-zero underflow.

The expanded fixture calls the composition in both Print constructor orders.
Replay saved iteration-84 response boundaries with
`--fased-timing-cycle-boundary` and
`--fased-timing-cycle-boundary-reverse`. Each checks nine atomic rejections:
helper/wrapper collisions, stale/malformed allocation, unsupported ID/data
widths, wrong top, missing annotation archive and repeated composition.
Every retained port keeps its type and direction; all five added ports have
the expected types and directions.

`PrintRocketFASEDTimingCycleCompare.py` pins the immutable U250
`design/FireSim-generated.sv` equations in `LatencyPipe` and gated model
clock/reset wiring in `FASEDMemoryTimingModel`, then interprets emitted native
SSA. Per constructor order it checks 22,048 transitions: boundary values,
10,000 random states and 10,000 continuous state steps. It retains all 235
preceding module definitions and the complete retargeted annotation archive,
checks all 172 wrapper connections, and requires the helper to match fresh
native compilation's `post-fame-fased-timing-cycle.mlir` from immutable SFC
`.sfc.fir`/`.anno.json`. The shared SSA evaluator supports unsigned FIRRTL pad.

Evidence is in mutable `iteration85-print-rocket-fased-timing-cycle/`.
Both expanded replays, the structured comparison and focused Print binding
and timing-cycle CTests pass. Per order, coverage includes 10,941 stalls,
5,569 stalled resets, 5,549 enabled resets, 64 counter wraps, 64 deadline
underflows and 710 deadline overflows. The native timing-cycle unit test
passes 21,152 transitions, 16 wrapper mappings, three target transfers and
19 atomic rejections. Fresh native ingestion, host assembly and RTL emission
pass; compiler, replay and comparison stderr logs are empty.

The expanded Print-enabled assembly remains partial and is exercised through
the compiler library and boundary fixture. Runtime latency register attachment,
completion queues, remaining timing policy, host-memory attachment and
Print-enabled production CLI/driver integration remain to compose. The golden
reference disables Print; the UART-bearing SFC runtime baseline remains
pending. Next smallest step: compose native `FASEDReadLatency` and compare
the ten-entry completion queue, empty flow-through, unsigned deadline test,
target-qualified reset/state and AR acceptance with SFC LatencyPipe/Queue_16.
Manager gates remain owned by the verification harness; this does not establish
overall port completion.

### Expanded Print/Rocket FASED read latency (iteration 86)

`mapPrintBridgeRocketFASEDReadLatency` composes `GGFASEDReadLatency10` and
`GGFASEDReadLatencyWrapper` after the expanded `GGFASEDTimingCycleWrapper`.
The native FIRRTL memory holds ten read deadlines and ID/length metadata.
It consumes `fased_read_release_cycle` and `fased_next_read`, connects accepted
AR requests to the queue, and connects released metadata to the existing
response releaser. All remaining ports retain their types and directions.
The original module operations, Print banks, and FASED allocation survive the
atomic composition. Invalid allocation, constructor widths, archive, top name,
module collisions, and repeat composition fail without changing the circuit.

The semantic oracle is Scala `models/dram/LatencyBandwidthPipe.scala:77–88`,
with `Queue_16`, `LatencyPipe`, and `FASEDMemoryTimingModel` in the immutable
U250 fixture's `design/FireSim-generated.sv`. The queue flows through when
empty and its unsigned 64-bit deadline is due. A full queue cannot accept a
replacement on the same edge that pops its head. AR acceptance remains
independent of queue capacity; the existing overflow assertion diagnoses a
missing upstream limit. Pointer/reset and RAM writes advance only on target
fire. Enabled reset clears the pointers/full bit but does not clear RAM or
suppress a simultaneous RAM write. The native memory preserves Scala's
8-bit length field, which the SFC reference optimizes out of its read queue.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration86-print-rocket-fased-read-latency/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-read-latency-boundary INPUT_TIMING_CYCLE_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-read-latency-boundary-reverse INPUT_REVERSE_TIMING_CYCLE_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDReadLatencyCompare.py EVIDENCE GOLDEN_SV ITERATION85_EVIDENCE`
pins the SFC queue/deadline/gated-clock equations, checks actual CIRCT SSA and
memory geometry, and verifies complete prior module and annotation retention.
Both constructor orders cover all ten dequeue pointers, all eleven occupancy
levels, reset/fire/valid/ready combinations, and unsigned deadline boundaries,
plus 30,000 continuous randomized transitions per order: 47,600 transitions,
176 wrapper connections, all 237 existing modules, and the complete annotation
archive pass per order. Both expanded replays reject nine malformed/repeated
compositions atomically. The standalone `goldengate-fased-read-latency` CTest
separately interprets 30,093 native FIRRTL memory/register transitions, including
63 reset writes, 1,399 full-queue pops, 99 stalled resets, and 860 pointer wraps.
The native build, both relevant CTests, and fresh immutable SFC handoff compile
through CIRCT RTL emission pass; compiler/replay/comparison stderr is empty.

This increment closes the expanded read-completion deadline boundary. Write
latency composition and attachment of the existing latency MMIO bank remain
subsequent steps; general FAME1/FAME5 and multi-clock support remain incomplete.

### Expanded Print/Rocket FASED write latency (iteration 87)

`mapPrintBridgeRocketFASEDWriteLatency` composes `GGFASEDWriteLatency10` and
`GGFASEDWriteLatencyWrapper` after the expanded `GGFASEDReadLatencyWrapper`.
The native FIRRTL memory holds ten 64-bit write deadlines and four-bit IDs.
It consumes `fased_write_release_cycle` and `fased_next_write`, takes the
completion pulse/oldest AW ID through `fased_write_completion`, exposes
capacity through `fased_write_latency_ready`, and sends due response metadata
to the existing releaser. Capacity is diagnostic; it does not gate the
completion pulse. All remaining ports retain their types and directions.
The original module operations, Print banks, FASED allocation and latency
register bank survive the atomic composition. Malformed allocation, unsupported
constructor widths, missing archive, wrong top, collisions and repeat
composition fail without changing the circuit.

The semantic oracle is Scala `models/dram/LatencyBandwidthPipe.scala:61–74`,
with `Queue_15`, `LatencyPipe` and `FASEDMemoryTimingModel` in the immutable
U250 fixture's `design/FireSim-generated.sv`. The queue flows through when
empty and its unsigned deadline is due. A full queue cannot accept a
replacement on the same edge that dequeues its head. Pointer/reset and RAM
writes advance only on target fire. Enabled reset clears queue state without
clearing RAM or suppressing a simultaneous RAM write. The queue diagnoses
overflow on an enabled, non-reset edge. SFC enqueues `newWReq` with the oldest
AW queue ID; generating that completion pulse and ID remains a later stage.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration87-print-rocket-fased-write-latency/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-write-latency-boundary INPUT_READ_LATENCY_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-write-latency-boundary-reverse INPUT_REVERSE_READ_LATENCY_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDWriteLatencyCompare.py EVIDENCE GOLDEN_SV ITERATION86_EVIDENCE`
pins the SFC queue/deadline/gated-clock equations and interprets actual native
SSA, including memory writes and assertion enable/predicate. It checks all ten
dequeue pointers, eleven occupancy levels, reset/fire/valid/ready combinations,
unsigned deadline boundaries and 30,000 continuous randomized transitions.
Per constructor order, 47,600 transitions, 174 wrapper connections, all 239
preceding module definitions and the complete retargeted annotation archive
pass. Coverage includes 203 flow-throughs, 1,646 full-queue pops, 18,822 stalls,
4,503 stalled resets, 1,990 reset writes, 1,172 pointer wraps, 8,958 overflow
assertion cases and 6,516 equal deadlines. The helper exactly matches fresh
native compilation's `post-fame-fased-write-latency.mlir` from immutable SFC
`.sfc.fir`/`.anno.json`; the fixture contains no Print host.

The native build, both expanded replays with nine atomic rejection cases each,
focused Print binding/write-latency CTests and fresh SFC handoff compilation
through CIRCT RTL emission pass. The standalone write-latency CTest interprets
30,093 transitions, 16 wrapper mappings, five target transfers and 21 atomic
rejections. Compiler, replay and final comparison stderr logs are empty.

This closes the expanded write deadline queue boundary. Next smallest step:
compose native `FASEDTimingAWQueue` to derive the oldest AW ID, comparing its
ten-entry flow-through queue and completion binding with SFC `Queue_14` and
`LatencyPipe`; AW/W pairing counters follow that stage. Latency MMIO attachment,
remaining timing policy, host-memory attachment and Print-enabled production
CLI/driver integration remain incomplete. General FAME1/FAME5 and multi-clock
support and the UART-bearing SFC runtime baseline remain outstanding. Manager
verification stays owned by the harness; this is not overall port completion.

### Expanded Print/Rocket FASED timing AW queue (iteration 88)

`mapPrintBridgeRocketFASEDTimingAWQueue` composes `GGFASEDTimingAWQueue10`
and `GGFASEDTimingAWQueueWrapper` after the expanded write-latency boundary.
It enqueues each timing-model AW handshake and binds the oldest four-bit ID
into the existing write-completion metadata. The external completion metadata
port becomes `fased_write_pair_complete`; queue capacity is observable through
`fased_timing_aw_queue_ready`. Capacity does not alter request acceptance or
gate completion. All other ports, original module operations, expanded Print
banks, FASED control allocation and latency register bank are preserved.
Composition stages a clone and verifies FIRRTL before transferring new modules
and retargeting the annotation archive. Collisions, malformed allocation,
unsupported widths, missing archive, wrong top and repeated composition reject
atomically.

The executable oracle is Scala `models/dram/TimingModel.scala:234–254` and
`Queue_14`, `LatencyPipe`, `FASEDMemoryTimingModel` in the immutable U250
fixture's `design/FireSim-generated.sv`. SFC optimizes unused AW metadata away
for this no-LLC profile. Its ten-entry ID queue flows through while empty,
wraps pointers at nine and cannot replace a full head on the same edge that
pops it. State/reset/RAM/assertion enable advance only on target fire. Enabled
reset clears pointers/full state while retaining RAM and allowing simultaneous
RAM writes. Overflow is diagnosed on an enabled non-reset edge.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration88-print-rocket-fased-timing-aw-queue/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-timing-aw-queue-boundary INPUT_WRITE_LATENCY_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-timing-aw-queue-boundary-reverse INPUT_REVERSE_WRITE_LATENCY_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDTimingAWQueueCompare.py EVIDENCE GOLDEN_SV ITERATION87_EVIDENCE`
pins the SFC queue/FIFO and gated-clock equations, then interprets actual native
SSA including memory writes and assertion predicates. It checks all ten
dequeue pointers, eleven occupancies, all reset/fire/valid/ready combinations,
all sixteen incoming IDs and 30,000 continuous randomized transitions in each
constructor order. It also compares all previous module definitions, the
complete retargeted annotation archive and every wrapper connection with the
fresh compiler's helper from immutable SFC `.sfc.fir`/`.anno.json` ingestion.
The immutable fixture contains no Print host.

Per constructor order, 58,160 transitions, 175 wrapper connections and all
241 preceding module definitions pass, with the complete retargeted annotation
archive preserved. Coverage includes 570 flow-throughs, 2,987 full-queue pops,
24,102 stalls, 7,149 stalled resets, 3,153 reset writes, 2,533 pointer wraps and
3,830 overflow assertion cases. The helper exactly matches fresh native
compilation's `post-fame-fased-timing-aw-queue.mlir`.

Native build, both expanded replays with nine atomic rejections each, focused
Print binding/timing-AW-queue CTests and fresh SFC handoff compilation through
CIRCT RTL emission pass. The standalone AW queue CTest checks 30,092
transitions, 64 wrapper mappings, five target transfers and 22 atomic rejections.
Compiler, replay and comparison stderr logs are empty.

This closes the expanded oldest-AW-ID boundary. Next smallest step: compose
native `FASEDWritePairing` to derive the no-LLC `newWReq` pulse from accepted AW
and last-W handshakes, comparing the saturating counters and pairing equations
with SFC `LatencyPipe` and `SatUpDownCounter` variants. Latency MMIO attachment,
remaining timing policy, host-memory attachment and Print-enabled production
CLI/driver integration remain incomplete. General FAME1/FAME5, multi-clock
support and the UART-bearing SFC runtime baseline remain outstanding. Manager
verification stays owned by the harness; this is not overall port completion.

### Expanded Print/Rocket FASED AW/W pairing (iteration 89)

`mapPrintBridgeRocketFASEDWritePairing` composes `GGFASEDWritePairing` and
`GGFASEDWritePairingWrapper` after the expanded timing AW queue boundary.
Accepted timing-model AW handshakes and accepted final W beats increment
separate four-bit saturating pending counters. Target B handshakes retire
both counters. The no-LLC `newWReq` expression consumes the prior external
`fased_write_pair_complete` port and drives the existing oldest-AW-ID queue
and write-latency completion path. Counter state and reset advance only on
target fire; the combinational completion expression remains ungated, as in
SFC. Runtime maximum reductions retain pending state rather than clamping it.

B retirement and maximum remain explicit `fased_target_b_fire` and
`fased_write_max_reqs` inputs. `fased_pending_writes` exposes both values and
full flags. All other ports, original module operations, expanded Print banks,
FASED control allocation and latency register bank remain intact. Composition
stages a clone and verifies FIRRTL before transferring new modules and
retargeting the annotation archive. Collisions, malformed allocation,
unsupported widths, missing archive, wrong top and repetition reject atomically.

The executable oracle is Scala `models/dram/TimingModel.scala:108–114,227–254`
and `widgets/Lib.scala:71–86`. The exact immutable RTL compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `SatUpDownCounter_1`, `LatencyPipe`
and `FASEDMemoryTimingModel`. Both pending counters instantiate the same
four-bit SFC counter module. The fixture contains no Print host.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration89-print-rocket-fased-write-pairing/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-write-pairing-boundary INPUT_TIMING_AW_QUEUE_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-write-pairing-boundary-reverse INPUT_REVERSE_TIMING_AW_QUEUE_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDWritePairingCompare.py EVIDENCE GOLDEN_SV ITERATION88_EVIDENCE`
pins the SFC counter, pairing and gated-clock equations, then interprets native
FIRRTL SSA. Per constructor order, all 16 AW states, 16 W states, 16 runtime
maximum values and 32 reset/fire/handshake combinations plus 30,000 continuous
randomized transitions pass: 161,072 transitions total. Coverage includes
18,123 AW-ahead pairings, 17,916 W-ahead pairings, 40,263 simultaneous pairings,
8,748 saturation cases, 3,343 empty retirements, 19,807 simultaneous increment
and decrement cases, 88,310 states above a reduced maximum, 75,440 stalls,
32,885 stalled resets and 32,982 target-qualified resets.

Both orders retain all 243 preceding module definitions, all 176 wrapper
connections match, and the complete retargeted annotation archive passes.
The helper exactly matches fresh native compilation's
`post-fame-fased-write-pairing.mlir` from the immutable compiler fixture's
`.sfc.fir` and `.anno.json`. Native build, both expanded replays with nine
atomic rejections each, focused Print binding/write-pairing CTests and fresh
SFC handoff compilation through CIRCT RTL emission pass. The standalone
pairing CTest checks 151,072 transitions, 256 wrapper mappings, five target
transfers and 23 atomic rejections. Compiler, replay and comparison stderr
logs are empty.

Next smallest step: compose native `FASEDWriteRetirement` into this expanded
boundary and compare its B-channel ready/valid attachment with SFC. Request
admission, latency MMIO attachment, remaining timing policy, host-memory
attachment and Print-enabled production CLI/driver integration remain
incomplete. General FAME1/FAME5, multi-clock support and the UART-bearing SFC
runtime baseline remain outstanding. Manager verification stays owned by the
harness; this is not overall port completion.

### Expanded Print/Rocket target B retirement (iteration 90)

`mapPrintBridgeRocketFASEDWriteRetirement` composes the existing native
`bindFASEDWriteRetirement` after the expanded Print/Rocket AW/W pairing
boundary. Both pending write counters now retire on the response releaser's
accepted target B handshake. This is `b.ready && b.valid`, independent of
reset and `targetFire`; counter state/reset still advance only on targetFire.
Host B acknowledgements and egress request acceptance are distinct events.

The composer verifies the expanded allocation and recorded 35/64/4-bit
constructor profile, applies the native retirement pass to a cloned circuit,
and verifies FIRRTL before applying the same pass to the unchanged original.
The native pass has no rejection after mutation begins. This retains existing
module operations and all prior port indices/types/directions while appending
one internal observation output to each of six response wrappers. Five `sim`
instances are rebuilt with the appended result and their existing metadata
and users retained. The new outer wrapper consumes the external retirement
input and closes feedback from the observed handshake. Print banks, stream
bindings, the FASED latency bank and expanded MMIO allocation remain intact.

The first expanded replay exposed a stale constructor-profile case: the
native retirement pass validated the exact B port shape but accepted changed
constructor widths. The composer now rejects stale width metadata before
staging. Wrapper collisions, non-unique response instances, malformed
allocation, incompatible widths, wrong top, absent annotation archive and
repetition are atomic rejection cases.

The executable oracle is `models/dram/TimingModel.scala:108–114,130` and
`models/dram/Util.scala:347–354`, with the target-clock contract in
`passes/fame/FAMETransform.scala` and `widgets/Lib.scala` counter semantics.
The exact immutable RTL compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LatencyPipe`, `AXI4Releaser`,
`SatUpDownCounter_1` and `FASEDMemoryTimingModel`. The fixture contains no
Print host; the comparison covers shared retirement/counter semantics and
preservation of the expanded native Print boundary.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration90-print-rocket-fased-write-retirement/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-write-retirement-boundary INPUT_PAIRING_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-write-retirement-boundary-reverse INPUT_REVERSE_PAIRING_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDWriteRetirementCompare.py EVIDENCE GOLDEN_SV ITERATION89_EVIDENCE`
pins SFC target B retirement, response release, counter arithmetic and gated
clock equations. It checks all six native observation hops and the closed
feedback, interprets the pending counter's FIRRTL SSA across all four-bit
AW/W states and maximum values with reset/fire/AW/W/B-ready/B-valid flags,
and checks continuous randomized state transitions. It compares every prior
wrapper driver, unchanged module definitions, the complete retargeted
annotation archive and fresh native SFC-ingestion retirement output.

Result: both constructor orders pass 292,144 counter transitions each
(262,144 exhaustive cases plus 30,000 randomized transitions), including
73,046 accepted B observations, 35,208 observations during target stalls,
73,177 valid/backpressure cases, 72,952 ready-without-valid cases, 1,261 empty
retirements, 16,030 simultaneous increment/decrement cases, 65,758
qualified resets and 65,657 stalled resets. All 245 existing module identities
remain; 239 module definitions are unchanged. All 1,060 prior/observation
drivers across the six wrappers, 170 outer-wrapper connections and the
complete retargeted annotation archive pass. The pairing helper and observed
retirement wiring match fresh native compilation's
`post-fame-fased-write-retirement.mlir` from the immutable compiler fixture's
`.sfc.fir` and `.anno.json`.

Native build, both expanded replays with nine atomic rejections each, focused
Print binding/write-retirement CTests and fresh SFC handoff compilation
through CIRCT RTL emission pass. The standalone retirement CTest checks six
observation hops, retained users/metadata/targets and 20 atomic rejections.
Final compiler, replay and comparison stderr logs are empty.

Next smallest step: compose native `FASEDWriteAdmission` into this expanded
boundary and compare pending-full admission with SFC. Runtime maximum and
latency MMIO attachment, remaining timing policy, host-memory attachment and
Print-enabled production CLI/driver integration remain incomplete. General
FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime baseline
remain outstanding. Manager verification stays with the harness; this is
not overall port completion.

### Expanded Print/Rocket pending-full write admission (iteration 91)

`mapPrintBridgeRocketFASEDWriteAdmission` composes native
`bindFASEDWriteAdmission` after target B retirement. FIRRTL `NotPrimOp`s
close AW/W reverse readiness with the respective pending-full observations.
Both readiness values remain observable outputs; only their bundle flips
change. Valid and payload are forwarded without masking, and AR retains its
reverse readiness. Admission is combinational and independent of reset and
`targetFire`; accepted AW and final W handshakes increment the existing
pending counters, whose state and reset remain target-qualified.

The composer checks expanded allocation and the recorded 35/64/4-bit
constructor profile, stages the native pass on a clone, and verifies FIRRTL.
It transfers only the new wrapper and retargeted annotation archive into the
original circuit. All existing module operations, ports, users, Print banks,
latency bank and allocation retain their identities. Collision, instantiated
top, malformed allocation, stale widths, wrong top, absent annotation archive
and repetition reject atomically.

The executable oracle is `models/dram/TimingModel.scala:108–114,225–260`,
`widgets/Lib.scala` counter semantics and the target-clock contract in
`passes/fame/FAMETransform.scala`. The exact immutable RTL compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LatencyPipe`, `SatUpDownCounter_1`
and `FASEDMemoryTimingModel`. The fixture contains no Print host; this
comparison covers shared admission/counter semantics and the expanded native
Print boundary's preservation.

Evidence lives under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration91-print-rocket-fased-write-admission/`. Native replay modes are:

```text
goldengate-print-binding-test --fased-write-admission-boundary INPUT_RETIREMENT_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-write-admission-boundary-reverse INPUT_REVERSE_RETIREMENT_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDWriteAdmissionCompare.py EVIDENCE GOLDEN_SV ITERATION90_EVIDENCE`
pins SFC admission, counter increments/retirement, runtime maximum, saturated
counter arithmetic and gated clock equations. It reads actual FIRRTL wrapper
wiring and interprets the pending-counter helper over all four-bit AW/W states
and maximum values, all reset/fire/valid/last/B-ready/B-valid combinations and
continuous randomized transitions. It compares every prior module definition,
all wrapper connections, the complete retargeted annotation archive and fresh
native SFC-ingestion admission output. Native tests check exact port types,
including preserved AR flip and payload types, and retained module identities.

Result: both constructor orders pass 554,288 transitions each (524,288
exhaustive cases plus 30,000 randomized transitions), including 34,631
zero-maximum cases, 131,828 accepted AW requests, 67,774 accepted final W
beats, 145,297 blocked AW requests, 141,823 blocked W beats, 108,745
handshakes during target stalls, 22,716 simultaneous increment/retirement
cases and 131,173 stalled resets. All 246 existing module identities and
module definitions remain unchanged. All 169 outer port identities/types
(with exactly two ready-flip changes), 177 wrapper connections and the
complete retargeted annotation archive pass. The pending-counter helper and
admission wiring match fresh native compilation's
`post-fame-fased-write-admission.mlir` from the immutable compiler fixture's
`.sfc.fir` and `.anno.json`.

Native build, both expanded replays with nine atomic rejections each, focused
Print binding/write-admission CTests and fresh SFC handoff compilation through
CIRCT RTL emission pass. The standalone admission CTest checks combinational
full inversion, unmasked valid/payload, AR passthrough, target retargeting and
16 atomic rejections. Compiler, replay, CTest and comparison stderr logs are
empty. Build stderr contains two Ninja log-recovery warnings; compilation
and linking completed successfully.

Next smallest step: compose native `FASEDReadAdmission` into this expanded
boundary and compare pending reads, final-R retirement and AR readiness with
SFC. Runtime maximum and latency MMIO attachment, remaining timing policy,
host-memory attachment and Print-enabled production CLI/driver integration
remain incomplete. General FAME1/FAME5, multi-clock support and the
UART-bearing SFC runtime baseline remain outstanding. Manager verification
stays with the harness; this is not overall port completion.


### Expanded Print/Rocket pending-read admission (iteration 92)

`mapPrintBridgeRocketFASEDReadAdmission` composes native
`bindFASEDReadAdmission` after write admission. FIRRTL operations observe
`R.ready && R.valid && R.last` at the response releaser, append that observation
through eight existing modules, and feed the pending-read counter. Accepted AR
increments the counter; accepted final R decrements it. AR readiness is
`!full`, independent of target fire and reset. Counter state and reset advance
only on target fire. Simultaneous increment/decrement holds, empty retirement
saturates, and lowering the runtime maximum does not clamp existing state.
AW/W admission remains connected through the preceding wrapper.

The composer validates expanded control allocation and the recorded 35/64/4-bit
constructor profile, applies the native mutation to a clone, verifies FIRRTL,
then applies it to the original circuit while preserving module identities.
Existing port indices/types/directions remain intact in the eight observed
modules. The outer wrapper retains all 169 prior ports, changing only the AR
ready flip, and appends the four-bit read maximum and pending-read observation.
Print banks, FASED banks and control allocation remain unchanged. Collision,
shared response hierarchy, malformed allocation, stale widths, wrong top,
missing annotation archive and repetition reject without mutation.

Executable Scala references inspected: `models/dram/TimingModel.scala:104–106`
and `225–231`, `widgets/Lib.scala:71–86`, `passes/MidasTransforms.scala` pass
ordering and `passes/fame/FAMETransform.scala` channel/target-clock contract.
The exact immutable RTL compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LatencyPipe`, `AXI4Releaser`,
`SatUpDownCounter_1` and `FASEDMemoryTimingModel`. The LatencyPipe instance
named `SatUpDownCounter` uses module `SatUpDownCounter_1` with a four-bit
runtime maximum; the separate two-bit module named `SatUpDownCounter` is not
this pending-read oracle. The fixture contains no Print host: results cover
shared FASED semantics and preservation of the expanded native Print boundary.

Evidence resides under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration92-print-rocket-fased-read-admission/`. Native replay modes:

```text
goldengate-print-binding-test --fased-read-admission-boundary INPUT_WRITE_ADMISSION_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-read-admission-boundary-reverse INPUT_REVERSE_WRITE_ADMISSION_MLIR OUTPUT_PREFIX
```

`PrintRocketFASEDReadAdmissionCompare.py EVIDENCE GOLDEN_SV ITERATION91_EVIDENCE`
pins SFC admission, final-R retirement, saturated counter arithmetic and gated
clock equations. It interprets actual native counter SSA, checks all eight
observation connections and every prior driver, retains the complete annotation
archive with expected retargeting, and compares the counter/helper wiring with
fresh native SFC ingestion's `post-fame-fased-read-admission.mlir`.

Result: both constructor orders pass 46,384 transitions each (16,384 exhaustive
four-bit state/maximum/reset/fire/AR-valid/R-ready/R-valid/R-last cases plus
30,000 continuous randomized transitions). Each includes 9,997 accepted ARs,
5,815 accepted final Rs, 5,929 accepted nonfinal Rs, 13,061 blocked ARs,
49 empty retirements, 682 simultaneous increment/decrement cases,
23,239 states above the runtime maximum, 2,904 zero-maximum cases and 4,200
stalled resets. All 247 prior module identities remain intact; 239 unrelated
module definitions remain byte-identical. The eight observation hops retain
all existing drivers: 1,415 prior/observation connections and 181 outer wrapper
connections match. The complete retargeted annotation archive matches.

Native build, both expanded replays with nine atomic rejections each and
focused Print-binding/read-admission CTests pass (5.82 seconds). The standalone
read-admission test covers 27 atomic rejection cases. Fresh native compilation
of immutable `.sfc.fir`/`.anno.json` inputs reaches simulator RTL emission.
Compiler, replay, CTest and comparison stderr are empty; build stderr contains
two Ninja log-recovery warnings and compilation/linking completes successfully.
Manager verification was not launched by this implementation turn.

Next smallest step: compose native `FASEDRequestLimits` into this expanded
boundary to replace the explicit runtime read/write maximum inputs with the
allocated host-clock MMIO bank, then attach latency registers. Remaining timing
policy, host-memory attachment, Print-enabled production CLI/driver integration,
general FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime
baseline remain incomplete. This increment does not complete the port.

## Iteration 93: expanded Print/Rocket FASED request-limit bank attachment

`mapPrintBridgeRocketFASEDRequestLimits` closes both runtime admission maximum
inputs using the already allocated `GGFASEDRequestLimits` bank. It validates
the expanded Print allocation, stages native `attachFASEDRequestLimits` on a
clone and verifies FIRRTL before moving only the new wrapper into the original
circuit. Existing modules, ports, bank bodies, constructor metadata and control
regions retain their identities. The retained annotation archive is explicitly
retargeted; targets of consumed maximum inputs remain on the inner module.
The decoded two-word MCR fragment stays external for subsequent control fanout.

The bank uses host clock and host reset, resets both 32-bit registers to 10,
accepts decoded write-valid independently of target fire and byte strobes,
returns full-width readback, and drives only bits 3:0 into read/write admission.
This preserves `SplitTransactionMMRegIO.maxReqRegisters` in
`models/dram/TimingModel.scala:204`, `Widget.attachIO/genAndAttachReg` and
`MCRIO.bindReg` in `widgets/Lib.scala`. `MidasTransforms.scala` ordering and
`FAMETransform.scala` input-ready/output-valid and target-clock contracts were
also inspected; this does not alter FAME advancement.

The exact immutable oracle compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FASEDMemoryTimingModel`. Its two 32-bit
registers, reset/write priority, low-four-bit model connections and full-width
MCR readback at words 2/3 establish the semantic comparison. The SFC fixture
has no Print host, so Print-specific claims cover native preservation only.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration93-print-rocket-fased-request-limits/`. Replay the preceding expanded
boundary in each constructor order with:

```text
goldengate-print-binding-test --fased-request-limits-boundary INPUT_READ_ADMISSION_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-request-limits-boundary-reverse INPUT_REVERSE_READ_ADMISSION_MLIR OUTPUT_PREFIX
PrintRocketFASEDRequestLimitsCompare.py EVIDENCE GOLDEN_SV ITERATION92_EVIDENCE
```

The comparison interprets actual bank SSA (including vector subindices), pins
SFC equations and register updates, compares every new wrapper connection and
all existing module definitions, and checks complete annotation retargeting.
It also matches the bank and FASED bindings against fresh native ingestion's
`post-fame-fased-request-limits.mlir`.

Result: both constructor orders pass 52,768 transitions each (32,768 exhaustive
edge-value/reset/fire/write-mask/strobe cases and 20,000 continuous randomized
cycles). Each includes 52,679 writes, 3,378 zero-strobe writes, 17,062 writes
concurrent with reset, 36,295 writes carrying upper bits and 26,333 writes
during target stalls. All 249 prior module definitions remain byte-identical
and their native operation identities/ports remain intact. All 174 new wrapper
connections and 170 ports match; the complete retargeted annotation archive
matches. The reused bank and FASED bindings match fresh native SFC ingestion.

Native build, both expanded replays with nine atomic rejections each, and both
focused Print-binding/request-limit CTests pass (7.07 seconds). The standalone
request-limit test covers 32,768 cycles and 33 atomic rejection cases across
materialization/attachment and combined mapping. Fresh native compilation of
the immutable `.sfc.fir`/`.anno.json` handoff reaches simulator RTL emission.
Compiler, replay, CTest and comparison stderr are empty; successful builds
report Ninja log-recovery warnings. Manager verification remains harness-owned.

Next smallest step: attach the already allocated `FASEDLatencyRegisters` bank
to the expanded boundary. Remaining timing policy, MCR/control fanout,
host-memory attachment, Print-enabled production CLI/driver integration,
general FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime
baseline remain incomplete. This increment does not complete the port.


## Iteration 94: expanded Print/Rocket FASED latency-register attachment

`mapPrintBridgeRocketFASEDLatencyRegisters` attaches the already allocated
`GGFASEDLatencyRegisters` after request limits. It validates expanded Rocket/
Print control allocation, stages native attachment and verifies FIRRTL before
moving only the new wrapper into the original circuit. Existing module and
port identities survive. The annotation archive is retargeted explicitly;
consumed latency-input targets remain on the inner module. The two full-width
latency inputs are replaced by a decoded two-word MCR output.

The Scala oracle is `LatencyPipeMMRegIO` in
`models/dram/LatencyBandwidthPipe.scala:19`, `Widget.attachIO/genAndAttachReg`
and `MCRIO.bindReg` in `widgets/Lib.scala:281`. Both 32-bit registers reset to
30 on host reset; decoded write-valid updates them independently of strobes
or target advancement. Readback and timing-model outputs retain all 32 bits.
`MidasTransforms.scala` pass ordering and `FAMETransform.scala` readiness,
validity and target-clock contracts were inspected and remain unchanged.

The exact immutable comparison artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FASEDMemoryTimingModel`, specifically
`writeLatency`/`readLatency`, MCR words 0/1 and full-width model connections.
The fixture has no Print host; Print-specific assertions establish native
preservation rather than an SFC Print-enabled differential comparison.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration94-print-rocket-fased-latency-registers/`. Replay and compare using:

```text
goldengate-print-binding-test --fased-latency-registers-boundary INPUT_REQUEST_LIMITS_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-latency-registers-boundary-reverse INPUT_REVERSE_REQUEST_LIMITS_MLIR OUTPUT_PREFIX
PrintRocketFASEDLatencyRegistersCompare.py EVIDENCE GOLDEN_SV ITERATION93_EVIDENCE
```

Both constructor orders pass 52,768 transitions each: 32,768 exhaustive edge
cases and 20,000 continuous randomized cycles. Each covers 52,967 writes,
3,235 zero-strobe writes, 17,071 writes during reset, 44,775 writes carrying
upper bits and 26,461 writes during target stalls. All 250 prior module
bodies remain byte-identical and native identities/ports remain intact.
All 169 wrapper ports, 173 connections and the complete retargeted annotation
archive match. The reused bank and FASED wiring also match fresh native
SFC ingestion's `post-fame-fased-latency-registers.mlir`.

Native build, both expanded replays (nine atomic rejections each), and focused
Print-binding/latency-register CTests pass (7.44 seconds). The standalone
latency-register test covers 32,768 cycles, split/combined mapping equivalence,
early allocation and 34 atomic rejection cases. Fresh native compilation of
the immutable `.sfc.fir`/`.anno.json` reaches simulator RTL emission. Compiler,
replay, CTest and comparison stderr are empty; the initial build reports Ninja
log-recovery warnings. Manager verification remains harness-owned.

Next smallest step: attach the already allocated `FASEDFunctionalModelRegister`
bank, replacing the ingress relaxation input with its reset-zero, bit-zero
setting at global MMIO word 18. Decoded MCR fanout, remaining FASED banks,
host-memory attachment, Print-enabled production CLI/driver integration,
general FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime
baseline remain incomplete. This increment does not complete the port.

## Iteration 95: expanded Print/Rocket FASED functional-model register

`mapPrintBridgeRocketFASEDFunctionalModelRegister` attaches the already
allocated `GGFASEDFunctionalModelRegister` after latency registers. It
validates the expanded Rocket/Print control allocation, stages native
attachment and verifies FIRRTL before moving only the new wrapper into the
original circuit. Existing module/port identities and the bank definition
survive. The annotation archive is retargeted explicitly; the consumed
relaxation-input target remains on the inner module. The one-bit ingress
relaxation input is replaced by a decoded one-word MCR output.

The Scala oracle is `FuncModelProgrammableRegs` in
`models/dram/FASEDMemoryTimingModel.scala:208`, `Widget.attachIO/genAndAttachReg`
and `MCRIO.bindReg` in `widgets/Lib.scala:281`. The 32-bit MMIO register resets
to zero on host reset and updates on decoded write-valid, independently of
strobes and target advancement. Readback retains all 32 bits; only bit zero
drives ingress relaxation. `MidasTransforms.scala` ordering and
`FAMETransform.scala` readiness/validity/target-clock contracts were inspected.

The exact immutable comparison artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FASEDMemoryTimingModel`, specifically
`relaxFunctionalModel`, MCR word 18 (byte offset 72), reset/write priority,
full-width readback and bit-zero ingress connection. The fixture has no Print
host; Print-specific checks establish native preservation rather than an
SFC Print-enabled differential comparison.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration95-print-rocket-fased-functional-model-register/`. Replay and compare:

```text
goldengate-print-binding-test --fased-functional-model-register-boundary INPUT_LATENCY_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-functional-model-register-boundary-reverse INPUT_REVERSE_LATENCY_MLIR OUTPUT_PREFIX
PrintRocketFASEDFunctionalModelRegisterCompare.py EVIDENCE GOLDEN_SV ITERATION94_EVIDENCE
```

Both constructor orders pass 28,192 transitions each: 8,192 exhaustive edge
cases and 20,000 continuous randomized cycles. Each covers 13,986 writes,
924 zero-strobe writes, 2,380 writes during reset, 12,962 writes carrying
upper bits, 6,971 writes during target stalls and 12,317 observations where
upper bits are set but relaxation stays off. All 251 prior module bodies
remain byte-identical and native identities/ports remain intact. All 169
wrapper ports, 172 connections and the complete retargeted annotation archive
match. The reused bank and FASED wiring also match fresh native SFC ingestion's
`post-fame-fased-functional-model-register.mlir`.

Native build, both expanded replays (nine atomic rejections each), and focused
Print-binding/functional-model-register CTests pass (6.55 seconds). The
standalone functional-model register test covers 32,768 cycles,
split/combined mapping equivalence, early allocation and 33 atomic rejection
cases. Fresh native compilation of the immutable `.sfc.fir`/`.anno.json`
reaches simulator RTL emission. Compiler, replay, CTest and comparison stderr
are empty; the build reports a Ninja log-recovery warning. Manager verification
remains harness-owned.

Next smallest step: attach the allocated `FASEDResponseErrors` bank and close
its response-handshake observations in the expanded boundary. Decoded MCR
fanout, remaining FASED banks, host-memory attachment, Print-enabled production
CLI/driver integration, general FAME1/FAME5, multi-clock support and the
UART-bearing SFC runtime baseline remain incomplete. This increment does not
complete the port.

## Iteration 96: expanded Print/Rocket FASED response errors

`mapPrintBridgeRocketFASEDResponseErrors` attaches the already allocated
`GGFASEDResponseErrors` after the functional-model register. It validates
Rocket/Print allocation, stages native attachment, verifies FIRRTL, and moves
only the new wrapper into the original circuit. Existing module and port
identities, constructor keys and allocated banks survive. The retained
annotation archive is explicitly retargeted to the new circuit/copied ports.
The wrapper observes host R/B valid and response codes at its inputs and
ready at the inner simulator outputs, preserving accepted-handshake semantics.

The oracle is `models/dram/FASEDMemoryTimingModel.scala:580-587` and
`widgets/Lib.scala:281-298`. Host reset takes priority over response capture.
Accepted nonzero responses update two UInt<2> registers; successful or
blocked responses leave them unchanged. Preserve the executable SFC behavior:
B errors capture the current R response code, including zero. Both registers
are read-only, zero-extended to UInt<32> at global MCR words 19/20 (bytes
76/80). Read-valid and write-ready are constant one; writes assert outside
host reset. State updates are independent of target advancement.
`MidasTransforms.scala` ordering and `FAMETransform.scala` ready/valid and
clock contracts were inspected.

The exact immutable comparison artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FASEDMemoryTimingModel`, specifically
`rrespError`, `brespError`, capture/reset priority, MCR readback and read-only
assertions. Its optimized capture guards contain response-valid with no ready
term; arbitrary ready stalls are checked against Scala's explicit `.fire`
contract and the native FIRRTL operations. The fixture lacks a Print host;
Print-specific checks prove native preservation rather than SFC Print parity.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration96-print-rocket-fased-response-errors/`. Replay and compare:

```text
goldengate-print-binding-test --fased-response-errors-boundary INPUT_FUNCTIONAL_REGISTER_MLIR OUTPUT_PREFIX
goldengate-print-binding-test --fased-response-errors-boundary-reverse INPUT_REVERSE_FUNCTIONAL_REGISTER_MLIR OUTPUT_PREFIX
PrintRocketFASEDResponseErrorsCompare.py EVIDENCE GOLDEN_SV ITERATION95_EVIDENCE
```

Both constructor orders pass 85,536 transitions each: 65,536 exhaustive cases
and 20,000 continuous randomized cycles. Each covers 9,746 accepted R errors,
9,754 accepted B errors, 17,726 blocked errors, 7,333 B errors capturing a
different R code (2,427 capturing zero), 11,359 errors coinciding with reset,
8,813 captures during target stalls, 52,173 forbidden writes and 33,436 writes
with assertions suppressed by reset. All 252 prior module bodies remain
byte-identical, and their native identities/ports survive. All 170 wrapper
ports, 178 connections and the complete annotation archive match. The reused
bank and its observation wiring match fresh native ingestion's
`post-fame-fased-response-errors.mlir`. The shared SSA comparison evaluator
now supports `firrtl.neq` for the bank's error guards.

Native build, both expanded replays (nine atomic rejections each) and focused
Print-binding/response-error CTests pass (7.92 seconds). The standalone bank
test covers 32,768 cycles, host handshake wiring, targets and 18 atomic
rejections. Fresh native compilation of the immutable `.sfc.fir` and
`.anno.json` reaches simulator RTL emission. Successful compiler, replay,
CTest and comparison stderr are empty; build stderr contains two Ninja log
recovery warnings. An early comparison attempt lacked the still-running
compiler's boundary; its log is retained and the rerun after emission passes.
Manager verification remains harness-owned.

Next smallest step: expose accepted target R beats through the expanded
response hierarchy for the allocated `FASEDStatistics` bank. Its native
attachment intentionally adds an observation port through 13 prior wrappers;
port/instance updates must be committed atomically alongside the new wrapper.
Decoded MCR fanout, remaining FASED banks, host-memory attachment, Print-enabled
production CLI/driver integration, general FAME1/FAME5, multi-clock support
and the UART-bearing SFC runtime baseline remain incomplete. This increment
does not complete the port.

## Iteration 97: expanded Print/Rocket FASED statistics

`mapPrintBridgeRocketFASEDStatistics` attaches the allocated statistics bank
at the completed response-error boundary. After validating Rocket/Print control
allocation, it stages native attachment and verifies FIRRTL on a clone. Native
attachment mutates thirteen existing modules and twelve `sim` instances to
carry `fased_accepted_r_fire` from the target response releaser. Moving only the
new wrapper would discard those mutations; instead the composer repeats the
fully preflighted native attachment on the original circuit. Native attachment
has no rejection paths after mutation starts. All existing module identities
and original port arguments, types, directions and order survive. The new
observation is consumed internally; the wrapper copies 170 original ports and
exports the four-word MCR fragment. The retained annotation archive is
explicitly retargeted. Hierarchy analyses must be recomputed afterward.

The executable oracle is `models/dram/TimingModel.scala:151-167`,
`FASEDMemoryTimingModel.scala:357-376` and `widgets/Lib.scala:281-298`.
AW/AR count transactions and W/R count every accepted beat, including non-final
R beats. The four UInt<32> counters wrap; targetFire qualifies increments and
model reset. Host reset suppresses read-only assertions independently and does
not reset these target-clocked counters. Global MCR words 14-17, bytes 56-68,
read the counters; read-valid and write-ready are constant one. Every write
asserts outside host reset, irrespective of strobe. The pipeline order in
`MidasTransforms.scala` and the ready/valid and clock contracts in
`FAMETransform.scala` were inspected.

The exact immutable comparison artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LatencyPipe` and
`FASEDMemoryTimingModel`. Counter widths, handshake guards, increment/reset
priority, model clock/reset bindings, readback and read-only assertions match.
The fixture lacks a Print host; Print checks prove native preservation rather
than SFC Print parity. The reused bank and event/observation wiring also match
fresh ingestion of the immutable compiler fixture's `.sfc.fir` and `.anno.json`
at `candidate/post-fame-fased-statistics.mlir`.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration97-print-rocket-fased-statistics/`. Replay and compare:

```text
goldengate-print-binding-test --fased-statistics-boundary INPUT_RESPONSE_ERRORS OUTPUT_PREFIX
goldengate-print-binding-test --fased-statistics-boundary-reverse INPUT_RESPONSE_ERRORS_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDStatisticsCompare.py EVIDENCE SFC_SV ITERATION96_EVIDENCE
```

Both constructor orders pass 36,384 transitions: 16,384 exhaustive handshake,
reset, target-fire, last-bit and counter-edge cases plus 20,000 randomized
cycles. Each includes 1,350 wraps, 1,728 accepted non-final R beats, 9,160 blocked
R beats, 4,391 stalled model resets, 13,840 host-only resets, 32,253 read-only
write violations and 40,481 reset-suppressed writes. All 253 module identities
and original arguments survive; 240 module bodies remain byte-identical. Each
of the thirteen expanded response wrappers preserves its original drivers,
adds exactly one observation port/driver and matches the native baseline.
All 170 copied ports, 179 wrapper connections and archive retargeting match.
Each expanded replay passes twelve atomic rejection cases, including duplicate
response hierarchy uses, a missing hierarchy module and observation collision.

Native build, both replays and focused Print-binding/statistics CTests pass
(9.11 seconds). The statistics unit test covers 32,768 cycles and 59 atomic
rejection cases. Fresh native compilation reaches simulator RTL emission.
Compiler, replay, CTest and comparison stderr are empty; the build reports two
Ninja log-recovery warnings. FireSim manager verification remains harness-owned.

Next smallest step: attach the allocated `FASEDHistograms` bank to this
statistics boundary using the exposed pending read/AW counts. Global MCR fanout,
host-memory attachment, Print-enabled production CLI/driver integration, general
FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime baseline remain
incomplete. This increment does not complete the port.

### Iteration 98: expanded Print/Rocket occupancy histograms

`mapPrintBridgeRocketFASEDHistograms` attaches the allocated native histogram
bank after the expanded statistics boundary. It requires the recorded Rocket
allocation, ten-flight constructor, retained annotation archive, unused bank
and exact clock/reset/fire and pending-count observation types. It stages the
native attachment and verifies the clone before moving only the new wrapper
into the live circuit. No existing module changes; prior module, port and
argument identities survive. Copied top targets explicitly transfer in the
archive. Hierarchy analyses must be recomputed after attachment.

The wrapper samples pre-edge `fased_pending_reads.value` and
`fased_pending_writes.awValue`, not pending W or either full flag. The oracle
`models/dram/TimingModel.scala:169-192` increments only the first matching bound
in 0, 2, 4, 8. Scala's `bins.zip(maximums)` leaves each fifth bin zero; preserve
that executable behavior even for counts above eight. Eight UInt<32> counters
wrap and reset synchronously only when model reset and targetFire coincide.
Host reset suppresses all ten read-only assertions and does not reset counters.
The ten MCR lanes retain global words 4-13, bytes 16-52; read-valid and
write-ready are one and writes assert independently of strobe. Clock/reset
qualification follows `FASEDMemoryTimingModel.scala:357-376`; MMIO permissions
follow `widgets/Lib.scala:281-298`. The existing `MidasTransforms.scala` pass
order and `FAMETransform.scala` clock/ready-valid semantics were inspected.

The exact immutable comparison artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LatencyPipe`, `FASEDMemoryTimingModel`
and `MCRFile_5`. Counter widths, first-fit increment guards, reset priority,
targetFire clock-gate enable, readback, constant fifth lanes and all ten
read-only assertions match. The fixture has no Print host; Print checks prove
native preservation rather than SFC Print parity. The allocated bank and seven
observation/control connections also match fresh ingestion of the immutable
compiler fixture's `.sfc.fir` and `.anno.json` at
`candidate/post-fame-fased-histograms.mlir`.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration98-print-rocket-fased-histograms/`. Replay and compare:

```text
goldengate-print-binding-test --fased-histograms-boundary INPUT_STATISTICS OUTPUT_PREFIX
goldengate-print-binding-test --fased-histograms-boundary-reverse INPUT_STATISTICS_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDHistogramsCompare.py EVIDENCE SFC_SV ITERATION97_EVIDENCE
```

Both constructor orders pass 28,192 transitions: 8,192 exhaustive occupancy,
reset, target-fire and counter-edge cases plus 20,000 randomized cycles. Each
includes 1,169 wraps, 9,467 above-last-bound samples, 2,388 stalled model resets,
11,713 host-only resets, 68,329 read-only write violations and 72,526
reset-suppressed writes. All 254 prior module bodies remain byte-identical;
all 171 copied ports, 178 wrapper connections and archive retargeting match.
Each replay passes ten atomic rejection cases, including missing/used bank,
used top, malformed allocation, constructor mismatch and repeat attachment.

Native build, both replays and focused Print-binding/histogram CTests pass
(16.05 seconds). The histogram unit test covers 32,768 cycles and 50 atomic
rejections. Fresh native compilation reaches simulator RTL emission. Compiler,
replay, CTest and completed comparison stderr are empty; the build reports two
Ninja log-recovery warnings. FireSim manager verification remains harness-owned.

Next smallest step: assemble these six exported FASED fragments into the native
21-word MMIO bank, then attach its MCRFile to the expanded control fabric.
Host-memory attachment, Print-enabled production CLI/driver integration,
general FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime
baseline remain incomplete. This increment does not complete the port.

### Iteration 99: expanded Print/Rocket FASED MMIO assembly

`mapPrintBridgeRocketFASEDMMIOBank` composes the native `addFASEDMMIOBank`
after histograms. It validates the expanded Rocket/Print control allocation,
six exact typed fragments, retained annotation archive and ten-flight
constructor, stages assembly on a clone, and verifies before moving the new
wrapper into the live circuit. Existing modules, ports, argument identities,
bank state and constructors remain unchanged. Hierarchy analyses must be
recomputed. Copied top targets and indexed fragment targets explicitly transfer;
whole-fragment references remain on the retained histogram wrapper.

The new top consumes six fragment ports and exposes one `fasedBridge_mcr`
bundle. Twenty-one read/write lanes follow the registry byte offsets, independently
of local row order. Six four-bit write strobes fan out to the banks. Read-ready
flows opposite read-valid/bits; write-ready flows opposite write-valid/bits.
The map retains byte offsets 0-80, five writable words (0-3 and 18), sixteen
read-only words, and both constant-zero histogram lanes (8 and 13). No state is
duplicated. This implements `FASEDMemoryTimingModel.scala:584-589`,
`Widget.scala:70-101,167-172` and `Lib.scala:200-236,281-298` through CIRCT
FIRRTL operations; the Midas pipeline and FAME clock/handshake oracle were
also inspected.

The exact immutable artifact compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `FASEDMemoryTimingModel` and `MCRFile_5`.
All 21 word identities, readback, writable/read-only guards, zero lanes and
four-byte address stride match. All 48 fragment routes and the ordered registry
also match fresh compilation of the immutable compiler fixture's `.sfc.fir`
and `.anno.json` at `candidate/post-fame-fased-mmio.mlir`. The SFC fixture lacks
Print hosts; expanded checks establish native Print preservation, not SFC Print
runtime parity.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration99-print-rocket-fased-mmio/`. Replay and compare:

```text
goldengate-print-binding-test --fased-mmio-boundary INPUT_HISTOGRAMS OUTPUT_PREFIX
goldengate-print-binding-test --fased-mmio-boundary-reverse INPUT_HISTOGRAMS_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDMMIOBankCompare.py EVIDENCE SFC_SV ITERATION98_EVIDENCE
```

Both constructor orders preserve all 255 prior module bodies, 166 copied ports
and the explicitly retargeted archive; all 214 wrapper connections match.
Each expanded replay passes twelve atomic rejection cases: wrapper collision,
used top, malformed allocation, constructor mismatch, wrong top, missing archive,
missing bank/fragment, duplicate offset, wrong permissions and repeat assembly.
The focused Print-binding and MMIO-bank CTests pass (5.84 seconds). The native
bank test independently checks three lane orders, 59 annotation targets per
order and 93 atomic rejections. Fresh native compilation reaches simulator RTL
emission. Replay, CTest, compiler and comparison stderr are empty; the build
reports two Ninja log-recovery warnings. FireSim manager gates remain owned by
the verification harness.

Next smallest step: attach the native FASED MCRFile to this expanded bank and
bind it to the allocated control slave. Host-memory attachment, Print-enabled
production CLI/driver integration, general FAME1/FAME5, multi-clock support and
the UART-bearing SFC runtime baseline remain incomplete. This increment does
not complete the port.

### Iteration 100: expanded Print/Rocket FASED control transport

`mapPrintBridgeRocketFASEDControl` attaches the shared native MCRFile to the
completed expanded MMIO bank and binds its five Nasti channels to the slave
selected by the live control catalog. It validates the Rocket/Print allocation
and compares assembled register names and permissions, by byte offset, with
the six allocated bank fragments. Both attachment and slave binding run on a
clone; only verified new modules and the transferred annotation archive are
committed. A late missing slave, wrapper collision or stale assembled registry
therefore leaves the original circuit unchanged. Existing modules, port and
argument identities, bank state, Print interfaces and constructors survive.

The native `GGFASEDMCRFile` implements `Lib.scala:311-384`: independently
captured AW/W, captured address indices and IDs, one write commit, held B/R
transaction flags, live selected read data, zero legacy write strobe, and
reset-masked single-beat assertions. `Widget.scala:167-172,338-339` supplies
the decoded-bank attachment and allocated-slave binding oracle. MidasTransforms
and FAMETransform were also inspected; this composition follows the existing
native model/channel and clock analyses rather than moving FAME logic into a
text adapter.

Evidence is under `sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration100-print-rocket-fased-control/`. Replay and comparison commands:

```text
goldengate-print-binding-test --fased-control-boundary INPUT_MMIO OUTPUT_PREFIX
goldengate-print-binding-test --fased-control-boundary-reverse INPUT_MMIO_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDControlCompare.py EVIDENCE SFC_SV ITERATION99_EVIDENCE
```

The exact immutable artifact selected for comparison is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `MCRFile_5`, `FASEDMemoryTimingModel`,
`NastiRouter` and `FPGATop`. SFC omits Print hosts, so the local transport and
FASED platform control pins are its oracle; expanded allocation and Print
preservation are checked against native IR. The comparator also checks the
shared MCRFile body against fresh native ingestion of the immutable compiler
fixture's `.sfc.fir` and `.anno.json`, at
`candidate/post-fame-fased-control.mlir` and `candidate/post-fame-fased-bound.mlir`.

The structured comparison passes in both constructor orders: nine transaction
registers, four reset flags, capture/retirement priority, all 21 write predicates
and selected read words, the 128-byte FASED region at slave 1, all five control
channels and 20 surviving SFC platform pins match. Each order preserves all 256
prior module bodies and the exact annotation transfer; 170 attachment and 177
binding connections match, including 33 control routes and 23 consumed ports.
The expanded MCRFile body equals the freshly ingested baseline body. SFC
optimizes unused write-strobe capture away; the native zero strobe follows the
inspected Scala oracle rather than an emitted SFC strobe pin.

Both expanded replays pass fifteen atomic rejection cases each, including late
binding failures and stale names/permissions in an otherwise correctly sized
bank. The three focused Print-binding, simulation-master-control and FASED
control CTests pass (140.45 seconds). The shared MCRFile test checks 96,000
cycles across 1-, 21-, 32- and 33-word banks, reordered AW/W, stalled B/R,
host reset, every local index and burst assertions, plus 27 malformed-boundary
and repeat-map rejections. Fresh native compilation emits simulator RTL.
Replay, compiler, comparison and CTest stderr are empty. Build logs report
Ninja log recovery; no compiler error occurs. Manager verification remains
owned by the harness; iteration 99 supplied passing CIRCT replacertl and
Verilator workload results, with the UART-bearing SFC baseline still pending.

Next smallest step: bind the host control master to the expanded fabric, then
attach FASED host memory. Print-enabled production CLI/driver integration,
general FAME1/FAME5, multi-clock support and the UART-bearing SFC runtime
baseline remain incomplete. This increment does not complete the port.

### Iteration 101: expanded Print/Rocket host control master

`mapPrintBridgeRocketControlMaster` closes the completed expanded FASED control
binding with the native five-channel host `ctrl` bundle. It first checks the
live Rocket/Print allocation and rejects remaining exported widget controls,
request dispatcher slaves or response arbiter inputs. The shared
`bindControlMaster` then consumes 32 scalar master boundaries and aggregate AR
on a clone. Only the verified new wrapper and transferred annotation archive
are committed, preserving every prior operation, port and argument identity.
No new state is introduced: AW/W readiness and B/R response acceptance still
come from the existing route and tracker-backed arbiters.

The executable oracle is `Widget.scala:315-340`: size-sorted widgets bind to
interconnect slaves, then `ctrlInterconnect.io.masters(0) <> master` connects
the host. MidasTransforms and FAMETransform were also inspected; existing FAME
clock/token and bridge metadata survive this downstream composition.

Evidence directory:
`sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration101-print-rocket-control-master/`. Replays consume iteration 100's
actual `binding.mlir.rocket-fased-control[-reverse].mlir` boundaries:

```text
goldengate-print-binding-test --rocket-control-master-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-control-master-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketControlMasterCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION100/candidate/post-fame-control-master.mlir \
  --report-json EVIDENCE/rocket-control-master-comparison.json
```

The exact immutable SFC artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FPGATop` and its `ctrlInterconnect`
instance. Compare the complete 45-leaf host control ABI and all 24 surviving
SFC master pins; unused metadata is optimized away and B/R user bits are zero.
The expanded native master routes are also compared with iteration 100's
native ingestion of the immutable compiler fixture. Print is disabled in SFC;
its shared host ABI is an oracle for this boundary, not Print runtime parity.

Both constructor-order comparisons pass: 259 unchanged prior module bodies,
111 copied ports, 21 retained Print modules and both allocated Print regions
(slaves 8 and 9, starts 544 and 576). The new wrapper consumes 33 ports through
32 scalar bindings and aggregate AR, with all 144 actual connections checked.
The full retained archive and four added copied/internalized/circuit identity
probes transfer exactly. Shared native baseline master routes also match.

Native compiler/test builds pass. Print-binding and control-master CTests pass
in 5.58 seconds; both expanded replays pass 14 atomic rejection cases each,
including wrapper collision, used top, stale allocation, missing boundaries,
remaining slave controls and repeat mapping. Replay, comparison and CTest
stderr are empty. Build stderr only reports Ninja log recovery, with no compiler
errors. No manager verification steps were started by the implementation agent.

Next smallest step: attach FASED host memory to this expanded control-master
boundary. Print-enabled production CLI/driver integration, general FAME1/FAME5,
multi-clock support and the UART-bearing SFC runtime baseline remain incomplete.
This increment does not complete the port. FireSim manager verification remains
owned by the harness; supplied iteration 100 results pass CIRCT replacertl and
both Verilator suites (90,141 portable and 90,805 Rocket checks).

## Iteration 102: expanded Print/Rocket FASED host memory

`mapPrintBridgeRocketFASEDHostMemory` consumes the completed expanded
`GGControlMasterWrapper` split host request/read-response/write-response ports
through the shared native `bindFASEDHostMemory`. It validates the live Print
allocation, stages and verifies the assembly on a clone, then commits only the
new `GGFASEDHostMemoryWrapper` and transferred archive. Existing operation,
port and argument identities survive; the host control bundle and all model,
bridge, ingress, egress and error-capture state remain unchanged. Three split
ports become one five-channel AXI master before host address translation.

The executable oracle is `FASEDMemoryTimingModel.scala:298-325` and
`junctions/nasti.scala:565-617`, `AXI4NastiAssigner.toAXI4Slave`. AW/W/AR fields
come from ingress, responses return to egress, and unused response users are
`DontCare` (`firrtl.invalidvalue`). NASTI request user/region and W ID fields
have no corresponding AXI leaves. Mapped leaf and copied targets transfer to
the new top; whole split-bundle and discarded metadata targets remain on the
inner control-master module. MidasTransforms and FAMETransform were inspected;
this downstream composition preserves their prior clock/token semantics.

Evidence directory:
`sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration102-print-rocket-fased-host-memory/`. Replays consume iteration 101's
`binding.mlir.rocket-control-master[-reverse].mlir`:

```text
goldengate-print-binding-test --rocket-fased-host-memory-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-fased-host-memory-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDHostMemoryCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION100/candidate/post-fame-fased-host-memory.mlir \
  --report-json EVIDENCE/rocket-fased-host-memory-comparison.json
```

The exact immutable artifact is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `FASEDMemoryTimingModel`, local
`auto_to_host_dram_out` AXI boundary. Its 35 surviving ports and associated
ingress/egress/error nets provide the 35/64/4-bit pre-translation contract.
SFC removes two response-ready leaves. The candidate retains all 37 AXI leaves
and two DontCare response-user sinks. The final platform memory address ABI
is a later boundary; SFC disables Print, so this is not Print runtime parity.

Both constructor-order comparisons pass: all 35 surviving SFC leaf widths,
directions and ingress/egress/error connections match. All 39 native memory
routes (37 AXI leaves and two invalid user sinks) also match iteration 100's
native ingestion of the immutable compiler fixture. The new wrapper preserves
260 prior module bodies, copies 109 ports, and has 110 final ports with 148
checked connections. All 21 Print modules, both Print regions (slaves 8 and 9,
starts 544 and 576), the complete annotation archive and eight added target
probes survive exactly. C++ checks also preserve prior argument identities.

Native compiler/test builds pass. Print-binding and FASED-host-memory CTests
pass in 5.56 seconds; both expanded replays pass 15 atomic rejection cases
each, covering collision, instantiated top, stale allocation, missing split
ports/archive/engine/constructor, all three unsupported width fields, existing
assembled port and repeat mapping. Replay, comparison and CTest stderr are
empty. Build stderr contains only Ninja log recovery warnings. Supplied
iteration 101 harness results pass CIRCT replacertl, bare Verilator smoke and
both required suites (90,141 portable and 90,805 Rocket checks); these are
prior gate results, not Print-enabled validation of this increment.

Next smallest step: compose the native FASED address translation onto this
expanded memory master. Print-enabled production CLI/driver integration,
general FAME1/FAME5, multiclock support and the UART-bearing SFC runtime
baseline remain incomplete. No manager verification steps are started by the
implementation agent; the harness owns replacertl, Verilator and U250 gates.

## Iteration 103: expanded Print/Rocket FASED address translation

`mapPrintBridgeRocketFASEDAddressTranslation` composes the shared native
`addFASEDAddressTranslation` onto the expanded `GGFASEDHostMemoryWrapper`.
It validates the live Rocket/Print allocation, stages and verifies on a clone,
then commits only `GGFASEDAddressTranslation` and its wrapper plus the
transferred archive. All prior module operations, ports and argument identities
survive. The completed control fabric, two Print hosts, ingress/egress state,
clock/reset and bridge/channel constructors remain intact.

The executable oracle is `AXI4AddressTranslation.scala:24-51` and
`FPGATop.scala:241-255`: the only MainMemory_0 region spans virtual addresses
0x80000000 through 0x47fffffff and occupies 16 GiB starting at host address zero.
Both 35-bit AW/AR addresses add 0x380000000 and retain bits 33:0. The four
assertions check AW upper bound, AR upper bound, AW lower bound, AR lower bound
on the host clock when reset is inactive. Requests qualify checks with valid,
independently of ready. No state is introduced. Copied annotation targets
transfer to the new top; all pre-translation memory targets retain the inner
module because their address semantics differ from the translated boundary.
MidasTransforms and FAMETransform were inspected; their clock/token behavior
remains upstream of this SimulationMapping composition.

Evidence directory:
`sim/generated-src/xilinx_alveo_u250/`
`xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/`
`iteration103-print-rocket-fased-address-translation/`. Replays consume
iteration 102's `binding.mlir.rocket-fased-host-memory[-reverse].mlir`:

```text
goldengate-print-binding-test --rocket-fased-address-translation-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-fased-address-translation-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketFASEDAddressTranslationCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION100/candidate/post-fame-fased-address-translation.mlir \
  --report-json EVIDENCE/rocket-fased-address-translation-comparison.json
```

The exact immutable artifact compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, module `AXI4AddressTranslation`. Both orders
match all 72 surviving AXI port widths/directions, 34 passthrough equations,
two translated addresses, and four valid/reset assertion predicates and
messages. SFC optimizes away both B.ready ports; native retains that ready
passthrough. The helper also matches iteration 100's native ingestion of the
immutable compiler fixture. SFC disables Print, so these matches establish
the local memory contract rather than Print-enabled runtime parity.

Both expanded candidates pass 1,024 actual SSA address/bounds vectors each,
including both virtual limits, overflow/truncation, all valid/ready combinations
and host reset. The helper has 35 passthrough connections and two address
connections. The wrapper copies 109 ports, retains 110 final ports and has
113 checked connections. All 261 prior module bodies, 21 Print modules,
both Print regions (slaves 8/9 at 544/576), the full annotation archive and
seven explicit translation target probes are preserved.

Native compiler/test builds pass after correcting a NamedAttrList accessor in
the new test. Print-binding and FASED-address-translation CTests pass in 5.59
seconds; the latter includes 1,024 vectors and 15 atomic rejections. Both
expanded replays pass 19 atomic rejections, including helper/wrapper collision,
instantiated top, stale allocation, missing memory/clock/reset/archive,
unsupported region/address sets/widths, missing constructor/engine, duplicate
engine and repeat mapping. Replay, comparison and CTest stderr are empty;
successful build stderr contains only Ninja log recovery. Supplied iteration
102 harness feedback passes CIRCT replacertl, bare Verilator smoke and both
required suites (90,141 portable and 90,805 Rocket checks). Those prior gate
results do not validate enabled Print behavior in this increment.

Next smallest step: compose native FASED read deinterleaving between this
expanded memory master and translation, comparing complete-burst arbitration
and response backpressure against the immutable SFC AXI4Deinterleaver boundary.
Print-enabled production CLI/driver integration, general FAME1/FAME5,
multiclock support and the UART-bearing SFC runtime baseline remain incomplete.
The overall port is not complete. The harness owns manager verification;
no replacertl, metasim or U250 buildbitstream steps were started here.

## Iteration 104: expanded Rocket/Print read deinterleaving

`mapPrintBridgeRocketFASEDReadDeinterleaver` composes the shared native
sixteen-ID, eight-beat response deinterleaver between the expanded host master
and address translation. It validates the live Rocket/Print allocation, stages
and verifies the native transformation on a clone, moves only the new queue
and helper definitions, and replaces the unique internal master connection.
The active top, all 263 prior module operations, their ports/block arguments,
annotation archive, constructor metadata and existing control/clock bindings
retain identity. Hierarchy analysis is invalidated. No annotation classes are
consumed or produced. Unsupported constructors, missing bindings/archive,
stale allocation, instantiated tops, symbol collisions and repeated mapping
fail before live mutation.

The executable oracle remains Scala `MidasTransforms`/`FAMETransform` followed
by `SimulationMapping`/`FPGATop` and Rocket's `AXI4Deinterleaver`. Complete bursts
increment per-ID pending counts; accepted RLAST decrements them. Arbitration
uses next counts, chooses the lowest pending ID, and locks through stalls until
accepted RLAST. Each queue is eight entries of 71-bit ID/data/resp/last payload,
with asynchronous RAM reads and synchronous writes. Reset flushes pointer/full
state and pending/lock state, leaves RAM unreset, and does not gate RAM writes.

Mutable evidence is under the current U250 generated-source tree at
`iteration104-print-rocket-fased-read-deinterleaver/`. Reproduce from iteration
103's saved translation boundaries with:

```
goldengate-print-binding-test --rocket-fased-read-deinterleaver-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-fased-read-deinterleaver-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
goldengate-fased-read-deinterleaver-test --expanded-boundary OUTPUT
goldengate-fased-read-deinterleaver-test --expanded-boundary OUTPUT_REVERSE
python3 test/Transforms/PrintRocketFASEDReadDeinterleaverCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION81/candidate/post-fame-fased-read-deinterleaver.mlir \
  --report-json EVIDENCE/rocket-fased-read-deinterleaver-comparison.json
```

The exact immutable artifact compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `AXI4Deinterleaver` and `Queue_34`.
Both orders match its 73 surviving ports, 30 passthrough routes, sixteen
queue clock/reset/enqueue bindings, response payload selectors, pending-count
increments/decrements, 32 bounds assertions, lowest-ID arbitration and burst
locking. Queue geometry, pointer/full equations and ungated accepted RAM writes
match. SFC removes both B.ready ports and the constant in.R.ready port; native
retains those ready paths. Its queue/helper also exactly match iteration 81's
native ingestion of the immutable compiler fixture. SFC disables Print, so
this comparison establishes the local contract, not enabled-Print runtime parity.

Both expanded replays pass 20 atomic rejections, preserve all 262 prior module
bodies outside the changed top, all 110 top ports, 21 Print modules, allocation
and the archive, and check all 116 wrapper connections. The actual emitted
helper in each composed circuit passes 2,048 queue-state samples and 8,364
hierarchical SSA edges against independent FIFO/complete-burst models: 6,050
enqueues, 3,927 dequeues, 1,945 stalls, 186 full cycles, 721 simultaneous accepted
last retirement/enqueue cycles and 42 resets. Queue samples include 136 reset
writes, 258 pointer wraps and 61 RAM collisions. Compiler and test builds pass;
Print-binding and shared deinterleaver CTests pass in 24.09 seconds. Replay,
behavior, CTest and comparison stderr are empty. The build log only reports
Ninja log recovery warnings. Supplied iteration 103 harness feedback passes
CIRCT replacertl and required Verilator workloads; these prior results do not
establish Print-enabled runtime parity for this increment.

Next smallest step: compose the shared native FASED host memory buffer after
translation and compare the five AXI channel queues with the immutable SFC
boundary. Print-enabled production CLI/driver integration, general FAME1/FAME5,
multiclock support and the UART-bearing SFC runtime baseline remain incomplete.
No FireSim manager verification gates were started here. The port is incomplete.

## Iteration 105: expanded Rocket/Print host memory buffering

`mapPrintBridgeRocketFASEDHostMemoryBuffer` composes the shared native AXI
buffer after address translation on the expanded Rocket/Print master. The
Scala oracle is `MidasTransforms`/`FAMETransform`, followed by
`SimulationMapping`/`FPGATop`'s `AXI4Buffer` between translation and the host
crossbar. All five channels have independent depth-two, no-flow/no-pipe queues;
AW and AR share a definition but retain separate state. Payload widths are
63 bits for addresses, 73 for W, 6 for B and 71 for R. Host reset flushes
pointer/full state, leaves RAM unreset and permits accepted RAM writes on a
reset edge. Reads are asynchronous and writes are synchronous.

The composition validates the live Rocket/Print allocation and the complete
transformation on a clone before moving only the five new definitions into
the live circuit. It replaces the one translated-master connection and binds
the buffer to the existing host clock/reset sources. The active top, all 265
prior module operations, their ports/block arguments, constructor metadata
and annotation archive retain identity. No annotations are consumed or
produced; callers must rebuild hierarchy analysis after insertion. Colliding
symbols, unsupported constructor widths/constraints, missing archive/bindings,
stale allocation, instantiated tops, missing deinterleaver and repeated mapping
fail before live mutation.

Mutable evidence is under the U250 generated-source tree at
`iteration105-print-rocket-fased-host-memory-buffer/`. Replay iteration 104's
saved deinterleaver boundaries with:

```
goldengate-print-binding-test --rocket-fased-host-memory-buffer-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-fased-host-memory-buffer-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
goldengate-fased-host-memory-buffer-test --expanded-boundary OUTPUT
goldengate-fased-host-memory-buffer-test --expanded-boundary OUTPUT_REVERSE
python3 test/Transforms/PrintRocketFASEDHostMemoryBufferCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION81/candidate/post-fame-fased-host-memory-buffer.mlir \
  --report-json EVIDENCE/rocket-fased-host-memory-buffer-comparison.json
```

The exact immutable artifact compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `AXI4Buffer_1` and
`Queue_29`/`Queue_23`/`Queue_31`/`Queue_33`. Both orders match its 75 surviving
ports, five queue instances, 54 payload routes, host clock/reset bindings,
depth-two memory geometry, pointer/full equations and reset-independent RAM
writes. SFC removes `auto_in_b_ready` and `Queue_31.io_deq_ready` because the
B consumer is always ready; native retains B stalls. The actual native helper
and four queue definitions also match iteration 81's ingestion of the
immutable compiler fixture. The fixture disables Print, so this establishes
the local host-memory contract, not enabled-Print runtime parity.

Both expanded replays pass 23 atomic rejections, preserve all 264 prior module
bodies outside the changed top, all 110 top ports, 21 Print modules and the
archive, and check all 119 wrapper and 84 helper connections. Each actual
composed circuit passes 256 exhaustive queue-state samples and 6,000
five-channel hierarchical SSA edges against independent FIFO models: 17,380
accepted enqueues, 17,147 dequeues, 8,350 stalls, 6,765 full samples, 9,487
simultaneous enqueue/dequeue samples and 44 resets. Exhaustive samples cover
48 reset writes, 64 full states and 32 RAM collisions. Compiler/test builds
and Print-binding/shared-buffer CTests pass (12.38 seconds). Replay, behavior,
comparison and CTest stderr are empty; the build reports only Ninja log
recovery warnings. Supplied iteration 104 harness feedback passes CIRCT
replacertl and required Verilator workloads; these prior gates do not verify
the new Print composition.

Next smallest step: compose the shared native LoadMem/FASED host-memory write
arbiter onto this expanded buffered boundary and compare AW reservations,
source IDs and accepted WLAST retirement with SFC `AXI4Xbar`/`Queue_21`.
Print-enabled production CLI/driver integration, general FAME1/FAME5,
multiclock support and the UART-bearing SFC runtime baseline remain incomplete.
No FireSim manager verification gates were started here. The port is incomplete.

### Expanded Print/Rocket LoadMem engine attachment (iteration 106)

The iteration 105 translated/buffered Print boundary retains the nine-word
`loadmem_mcr` interface but has no LoadMem AXI engine pins. Replaying the native
host write arbiter there rejected the missing AW/W ports. The prerequisite is
now `mapPrintBridgeRocketLoadMemEngine`: it composes the existing native writer,
write-request queue/priority arbiter, MMIO bank, write-data FIFO, read-request
engine and read-data FIFO through CIRCT FIRRTL operations. It supplies the
sixteen 34-bit-address/64-bit-data AXI pins needed by the next arbiter step.
No writer, queue, register or FIFO state is duplicated in the composition.

`GGPrintLoadMemEngine` has 19 ports and seven helper instances. Its nine-word
MCR interface connects words 0/1/2/3/5 to the existing bank, word 4 to the
32-to-64 write FIFO, words 6/7 to the read-request engine and word 8 to the
64-to-32 read FIFO. The additional word-4 read and word-8 write permission
assertions are enabled outside host reset. The queued write request sets
`zero=false`; the existing arbiter gives queued writes priority over the
16-GiB zero-fill request. `ZERO_FINISHED` comes from writer request readiness.
The read-address concatenation/truncation behavior remains in the shared
native read-request helper and matches the Scala/SFC oracle.

`GGPrintLoadMemWrapper` retains 109 of the old 110 ports, consumes the MCR
port and adds sixteen memory ports, giving 125 ports and two instances.
Construction and verification happen on a clone before moving only these two
new definitions into the live circuit. All 270 previous module operations,
ports and SSA arguments retain identity. Copied top port targets move to the
new top; the consumed MCR target remains on the inner top. Circuit identities
are recursively rewritten in the raw annotation archive. Missing/ambiguous
catalogs, wrong catalog port names/types/directions, unsupported allocation,
missing archive/host/MCR interfaces, missing buffered hierarchy, instantiated
tops, symbol collisions and repeated composition reject before live mutation.
Callers must rebuild hierarchy/target analyses after the new top is installed.

Evidence is under the U250 generated-source tree at
`iteration106-print-rocket-loadmem-engine/`. Replay iteration 105's two saved
buffered boundaries with:

```
goldengate-print-binding-test --rocket-loadmem-engine-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-loadmem-engine-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
python3 test/Transforms/PrintRocketLoadMemEngineCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION81/candidate/post-fame-host-memory-write-arbiter.mlir \
  --report-json EVIDENCE/rocket-loadmem-engine-comparison.json
```

The exact immutable artifact compared is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `LoadMemWidget`,
`Nasti2AXI4IdentityModule`, `Arbiter` and `Queue_27`. The comparison checks
sixteen memory pins, 75 widget equations, sixteen identity-conversion routes,
write-first arbitration, zero-fill constants and the depth-two request queue.
It separately checks all 128 outer-wrapper and 80 engine connections,
unchanged prior module bodies, seven shared helper definitions against the
native iteration 81 boundary, all 21 Print modules and nested archive targets.
The fixture disables Print; this local composition does not establish
Print-enabled production runtime parity.

Next smallest step: adapt the native host write-arbiter composition to this
new LoadMem/FASED boundary and compare AW reservations, IDs and WLAST
retirement with SFC `AXI4Xbar`/`Queue_21`. Print-enabled production integration,
general FAME1/FAME5 and multiclock coverage remain incomplete. The harness owns
manager verification; no manager gate was started in this iteration.

Validation: both expanded annotation orders pass all 19 atomic rejection
checks and the complete structured comparison. The native compiler and Print
binding executable build; Print binding CTest passes in 5.21 seconds and the
six shared LoadMem semantic CTests pass in 30.51 seconds (161,864 modeled
cycles plus 4,096 arbiter cases). Successful replay/comparison stderr is
empty. An initial missing-archive negative test exposed a null-attribute
assertion; explicit archive preflight fixes it and the final replays cover
that case without mutation. Supplied iteration 105 harness feedback passes
CIRCT replacertl and required Verilator workloads; those earlier gates do
not verify this new Print-enabled composition. The overall port is incomplete.

### Expanded Print/Rocket host AW/W arbitration (iteration 107)

`mapPrintBridgeRocketHostMemoryWriteArbiter` now composes the native host write
arbiter and its depth-two flow source queue onto iteration 106's LoadMem
attachment. The shared compiler transform accepts either the original buffered
translation top or the expanded Print LoadMem top. For Print it validates the
two-instance attachment, all sixteen direct LoadMem pins, MCR binding, both
host clock/reset routes and the retained four-instance buffered master before
constructing new operations. A staged clone is verified before moving only
three new definitions into the live circuit. Existing module, port and SSA
argument identities remain unchanged; hierarchy and target analyses need
rebuilding after installation.

The new `GGHostMemoryWriteWrapper` retains 116 ports and adds the remaining
FASED B/AR/R bundle and shared AW/W bundle, giving 118 ports and two instances.
All eight LoadMem AW/W pins and FASED AW/W connect to the shared arbiter.
LoadMem uses ID 16, full-byte strobes, size 3 and INCR burst; FASED retains
IDs 0..15, payload and strobes. The source queue reserves the AW winner before
AW acceptance, holds the reservation through stalls, and retires it only on
an accepted selected WLAST. Queue flow, non-pipelined full behavior and RAM
writes during reset remain in the existing native implementation.

Copied public targets and remaining B/AR/R targets move to the new top;
consumed AW/W targets remain on the inner LoadMem wrapper. Whole remaining
channel targets (for example `fased_host_mem.ar`) now transfer alongside their
leaves. Annotation circuit identities are recursively updated without
consuming annotation classes. The nested-target test covers copied, consumed,
remaining-leaf and whole-channel references.

Replay both saved LoadMem boundaries using:

```
goldengate-print-binding-test --rocket-host-memory-write-arbiter-boundary INPUT OUTPUT_PREFIX
goldengate-print-binding-test --rocket-host-memory-write-arbiter-boundary-reverse INPUT_REVERSE OUTPUT_PREFIX
goldengate-host-memory-write-arbiter-test --expanded-boundary OUTPUT
python3 test/Transforms/PrintRocketHostMemoryWriteArbiterCompare.py \
  --before INPUT --after OUTPUT --before INPUT_REVERSE --after OUTPUT_REVERSE \
  --sfc SFC_SV --baseline ITERATION81/candidate/post-fame-host-memory-write-arbiter.mlir \
  --report-json EVIDENCE/rocket-host-memory-write-arbiter-comparison.json
```

Evidence is under the U250 generated-source tree at
`iteration107-print-rocket-host-memory-write-arbiter/`. The immutable golden is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/`
`cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/`
`design/FireSim-generated.sv`, modules `AXI4Xbar` and `Queue_21`. The structured
comparison anchors the interpreter oracle to 46 crossbar equations and twelve
queue equations, plus the shared 34-bit-address/64-bit-data/five-bit-ID ABI,
queue depth, reset state and reset-independent RAM writes. It checks every
outer-wrapper route, unchanged prior module bodies, identical shared native
helper bodies, all 21 Print modules and nested archive target transfer.

Next smallest step: adapt the native host B response router to the retained
Print LoadMem wrapper, preserving ID-16 LoadMem and IDs-0..15 FASED selection,
then compare response valid/ready/ID behavior with the same SFC `AXI4Xbar`.
AR/R composition, Print-enabled production CLI/driver integration and runtime
parity, general FAME1/FAME5, multiclock support and the SFC UART baseline remain
incomplete. Supplied iteration 106 harness feedback passes CIRCT replacertl
and required Verilator workloads; it does not exercise this new Print-enabled
composition. No manager verification gate was started in this iteration.

Validation: compiler, Print binding and host write-arbiter executables build.
Focused CTests pass in 11.89 seconds (Print binding 5.46, arbiter 6.43).
Both expanded annotation orders pass 22 atomic rejections, full FIRRTL
verification and the structured golden comparison. Each actual candidate's
arbiter SSA passes 216,018 leaf checks across 12,001 edges, plus 256 exhaustive
source-queue cases. Trace coverage includes full queues, AW stalls, early
reservations, empty bypass, simultaneous enqueue/dequeue and 52 reset edges;
forty exhaustive cases check RAM writes during reset. Successful replay,
semantic and comparison stderr files are empty. A C++ test operand-setter
compile error was corrected before the final successful build; its failed
build log is retained. Existing shared-worktree changes remain outside this
checkpoint. The overall port remains incomplete.

### Iteration 108: expanded Print host B response composition

`mapPrintBridgeRocketHostMemoryWriteResponses` composes the shared native
`routeHostMemoryWriteResponses` FIRRTL router onto the expanded
`GGHostMemoryWriteWrapper`. The native preflight now accepts either the
original FASED address-translation child or `GGPrintLoadMemWrapper`; both
must forward the host clock/reset directly to the retained master and AW/W
reservation arbiter. The Print entry point also checks the live expanded
control allocation, stages both new modules on a clone and verifies the
result before moving only the additions to the live circuit. Existing module
operations, ports and SSA arguments retain their identities.

The new `GGHostMemoryWriteResponseWrapper` has 116 ports: 114 copied ports,
remaining FASED AR/R and the joined host AW/W/B master. Its 122 connections
preserve the LoadMem/FASED response flips and every copied control/Print port.
LoadMem B and FASED B targets remain on the retained inner top. Copied ports,
whole AW/W/AR/R channels and their leaves transfer to the new public top;
original aggregate targets stay inside. No annotation class is consumed.

The executable Scala pipeline in `MidasTransforms.scala` and host clock/reset
semantics in `FAMETransform.scala` were reread. The response oracle is
`FPGATop.scala`'s host AXI4Xbar and Rocket Chip `amba/axi4/Xbar.scala`'s ID
range trimming and response fanout. Exact immutable golden compared:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketHostMemoryWriteResponsesCompare.py` anchors seven `AXI4Xbar` B
selection/payload/ready equations and the 5-bit host, 4-bit FASED and 2-bit
response ABI. ID 16 selects LoadMem, IDs 0..15 select FASED with four-bit ID
truncation, and IDs 17..31 block both consumers. Host ready depends on the
selected consumer's ready independently of valid. LoadMem exposes only B
ready/valid; FASED receives the response bits unchanged.

Replay the prior iteration's actual expanded AW/W boundary with
`goldengate-print-binding-test --rocket-host-memory-write-responses-boundary
INPUT OUTPUT_PREFIX`, or the `-reverse` variant, after sourcing the FireSim
project environment. Then run
`goldengate-host-memory-write-responses-test --expanded-boundary OUTPUT` and
the structured comparison with `--before`, `--after`, `--baseline`, `--sfc`
and `--report-json`. Both orders exercise 19 atomic rejections, including
missing and duplicate clock/reset drivers, malformed interfaces, missing
B/AR/R bindings, constructor mismatches, collisions and repeated composition.
A separate nested-target probe checks twelve copied/consumed leaf, channel
and aggregate identities.

The structured comparison passes in both orders, preserves all 275 prior
module bodies including 21 Print modules, and matches the B router to the existing non-Print native
boundary in `iteration81-print-rocket-fased-read-buffer/candidate/
post-fame-host-memory-write-responses.mlir`. The SSA interpreter exhausts
1,024 ID/response/valid/two-ready combinations (5,120 leaf checks, including
480 unmatched-ID cases) on each emitted candidate. Evidence is under the
mutable generated Rocket directory's
`iteration108-print-rocket-host-memory-write-responses/`.

Next smallest step: compose the existing host AR arbiter on this expanded
Print boundary, preserving LoadMem read request and FASED R identities, then
compare its stalled selection and ID insertion with the recorded AXI4Xbar.
Expanded Print production CLI/driver integration and runtime parity, general
FAME1/FAME5, multiclock support and the SFC UART baseline remain incomplete.
Supplied iteration 107 harness feedback passes CIRCT replacertl and required
Verilator workloads; it does not exercise this newly expanded Print path.
No FireSim manager verification gate was started by the implementation agent.

Validation: the compiler, Print binding and native host write/read test
executables build. All four focused CTests pass in 17.04 seconds. Both
expanded replays, 19 rejection cases per order, FIRRTL verification,
1,024-case SSA comparisons and the structured golden comparison pass.
Comparison and semantic stderr logs are empty. Existing shared clock-control,
U250 recovery and harness edits are excluded from this checkpoint. The
migration remains incomplete.

### Iteration 109: compose host AR arbitration on the expanded Print boundary

`mapPrintBridgeRocketHostMemoryReadArbiter` attaches the shared CIRCT
`HostMemoryReadArbiter` pass to the actual expanded Print/LoadMem/FASED AW/W/B
boundary. It validates the expanded FASED allocation, stages and verifies the
transformation, then moves only the new wrapper and arbiter into the live
circuit. Existing module, port and SSA argument identities remain intact.
The shared pass now checks both LoadMem B scalar routes and the FASED B
aggregate route before attaching AR, and transfers whole retained FASED R
channel targets as well as their leaves.

The new `GGHostMemoryReadWrapper` has 113 ports: 111 copied ports, remaining
FASED R and the joined host AW/W/B/AR master. Its 122 connections preserve
all copied controls and Print interfaces, forward host clock/reset to the
arbiter, arbitrate LoadMem/FASED AR, and retain AW/W/B/R routing. Copied ports
and whole retained R targets transfer to the public top. Host AW/W/B channel
and leaf targets rename `host_mem_write` to `host_mem`; consumed AR targets
and old aggregate identities remain on the retained inner module.

The executable Scala pipeline in `MidasTransforms.scala` and host clock/reset
semantics in `FAMETransform.scala` were reread. The arbitration oracle is
`FPGATop.scala`'s AXI4Xbar, Rocket Chip `amba/axi4/Xbar.scala`'s `returnWinner`
and `tilelink/Arbiter.scala`'s round-robin policy. Exact immutable artifact:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketHostMemoryReadArbiterCompare.py` anchors 49 recorded `AXI4Xbar`
AR selection, ready and payload equations, seven synchronous state-update
anchors, and the 5-bit host / 4-bit FASED ID and 34-bit address ABI. Priority
rotates on idle selection, before acceptance; selection locks through stalls.
LoadMem emits ID 16, length 0, size 3 and burst 1. FASED IDs zero-extend to
five bits; all selected payload fields preserve the recorded Mux1H behavior.

Replay the actual iteration-108 expanded boundary with
`goldengate-print-binding-test --rocket-host-memory-read-arbiter-boundary
INPUT OUTPUT_PREFIX`, or its `-reverse` variant, after sourcing the project
environment. Both orders check 21 atomic rejections, including malformed
interfaces, collisions, constructor mismatch, missing AR/R/B bindings,
missing/duplicate host clock/reset and repeated attachment. A separate nested
target probe checks thirteen copied, consumed, channel and aggregate targets.
Then run `goldengate-host-memory-read-arbiter-test --expanded-boundary OUTPUT`
and the structured comparison with `--before`, `--after`, `--baseline`,
`--sfc` and `--report-json`. The native-helper baseline is
`iteration81-print-rocket-fased-read-buffer/candidate/
post-fame-host-memory-read-arbiter.mlir`; mutable evidence is under
`iteration109-print-rocket-host-memory-read-arbiter/`.

Next smallest step: compose the existing host R response router onto this
expanded Print boundary, preserving LoadMem read data/handshake and FASED R
identities, then compare ID selection, truncation and selected-consumer
backpressure against the same SFC AXI4Xbar. Expanded Print production
CLI/driver integration and runtime parity, general FAME1/FAME5, multiclock
support and the SFC UART baseline remain incomplete. Supplied iteration-108
harness feedback passes CIRCT replacertl and required Verilator workloads;
it does not establish runtime parity for this newly expanded Print path.
No FireSim manager verification gate was started by the implementation agent.

Validation: the compiler and Print/native host read test executables build.
All three focused CTests pass in 17.17 seconds. Both expanded replays and
21 atomic rejections per order pass. Each emitted arbiter passes 512
exhaustive state/input/reset cases (8,192 checks) and a 12,001-cycle trace
(192,016 checks, 5,056 stalled cycles and 5,143 contention cycles). FIRRTL
verification and the structured golden comparison pass in both orders;
all 277 prior module bodies including 21 Print modules are unchanged,
111 ports are copied, and 122 routes and recursive archive targets match.
The helper is identical to the native baseline. Comparison and semantic
stderr logs are empty. Existing shared clock-control, U250 recovery and
harness edits are excluded from this checkpoint. The port remains incomplete.

### Iteration 110: compose host R responses on the expanded Print boundary

`mapPrintBridgeRocketHostMemoryReadResponses` attaches the shared CIRCT
`HostMemoryReadResponses` pass to the expanded Print/LoadMem/FASED AW/W/B/AR
boundary. It checks the recorded expanded allocation, stages and verifies
IR, then moves only the new wrapper and router into the live circuit. Prior
module, port and SSA argument identities remain intact. The native pass now
counts both ordinary and strict connections for unique boundary/clock/reset
validation and transfers whole retained AW/W/B/AR channel targets as well
as their leaves. Consumed LoadMem R scalars, FASED R and original aggregate
targets remain on the retained inner module.

The new `GGHostMemoryReadResponseWrapper` has 109 ports: 108 copied ports and
one complete five-channel host master. Its 117 connections forward the
copied controls and Print interfaces, retain AW/W/B/AR, and route host R to
LoadMem data/handshake or the four-bit FASED R channel. The router is
combinational; ID16 selects LoadMem, IDs0..15 select FASED, and IDs17..31
stall. Ready depends on the selected consumer independently of host valid.
FASED ID truncates to four bits; payload is broadcast without valid gating.

`MidasTransforms.scala`, `FAMETransform.scala`, `FPGATop.scala`'s AXI4Xbar
and Rocket Chip `amba/axi4/Xbar.scala`'s fanout/ID trimming were reread.
Exact immutable comparison artifact:

```
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketHostMemoryReadResponsesCompare.py` anchors ten `AXI4Xbar` R
selection, ready, ID, data, status and last equations, and six port ABI
checks. The C++ interpreter independently evaluates each emitted router's
SSA for all 32 IDs, four statuses, both last/valid/consumer-ready values
and four 64-bit payload patterns. The structured comparison checks all
279 prior bodies (including 21 Print modules), typed ports, all routes,
recursive archive transfer and equality with the native R helper.

After sourcing the project environment, replay both iteration-109 boundaries
with `goldengate-print-binding-test
--rocket-host-memory-read-responses-boundary INPUT OUTPUT_PREFIX` and its
`-reverse` variant. Each checks 25 atomic rejections, including malformed
interfaces, constructor mismatch, collisions, missing AR/R/LoadMem data
bindings, missing/duplicate sim and arbiter clock/reset (including mixed
ordinary/strict duplicates), and repeated attachment. A nested probe checks
15 copied, channel, consumed and aggregate targets. Run
`goldengate-host-memory-read-responses-test --expanded-boundary OUTPUT` and
then the structured comparison with `--before`, `--after`, `--baseline`,
`--sfc` and `--report-json`. Native baseline:
`iteration81-print-rocket-fased-read-buffer/candidate/
post-fame-host-memory-read-responses.mlir`. Mutable evidence:
`iteration110-print-rocket-host-memory-read-responses/`.

Next smallest step: compose the existing five-channel host output buffer
onto this expanded Print boundary and compare queue occupancy, reset,
backpressure and payload stability against the SFC AXI4Buffer queues.
Expanded Print production CLI/driver integration and runtime parity,
general FAME1/FAME5, multiclock and the SFC UART baseline remain incomplete.
Supplied iteration-109 harness feedback passes CIRCT replacertl and the
required Verilator workloads; it does not establish runtime parity for this
new expanded Print path. No FireSim manager verification gate was started
by the implementation agent. Shared clock-control, U250 recovery and harness
edits remain outside this checkpoint.

Validation: the compiler and Print/native host read test executables build.
All three focused CTests pass (native R/AR: 11.56 seconds; Print: 5.51 seconds).
Both expanded replays, all 25 atomic rejections per order, and the nested
15-target probes pass. Each emitted helper passes 8,192 exhaustive cases
(65,536 leaf checks, including 3,840 unmatched-ID cases). FIRRTL verification
and structured golden comparison pass in both orders: ten SFC R equations
and six port ABI checks match, all 279 prior module bodies remain unchanged,
108 copied ports, 109 public ports and 117 routes match, and the R helper is
identical to the native baseline. All five replay/comparison/semantic stderr
logs are empty. The CIRCT port remains incomplete.

### Iteration 111: expanded Print host memory output buffer

`mapPrintBridgeRocketHostMemoryOutputBuffer` attaches FPGATop's final
AXI4Buffer to the expanded Print/Rocket unified master. The shared CIRCT
pass creates four queue definitions and five independent AW/W/B/AR/R
instances; AW and AR share a definition but retain separate state. Requests
flow toward host DRAM, while B and R flow toward the existing routers.
Each queue has depth two, asynchronous RAM read, and neither flow nor pipe.
Reset clears both pointers and occupancy; an accepted enqueue still writes
RAM on a reset edge, matching Chisel Queue. The native preflight now counts
both ordinary and strict clock/reset connects and rejects duplicate or
conflicting drivers before mutation.

The composition validates the recorded constructor/allocation and stages the
shared pass on a clone. After FIRRTL verification it moves only the five new
modules, adds the buffer instance and three connections, then redirects the
five existing host channel subfields in place. It retains all prior module,
port, SSA argument and top operation identities. The recursive annotation
archive stays identical; no target retargeting or consumption is needed.
The top remains `GGHostMemoryReadResponseWrapper` with 109 public ports,
three instances and 120 connections; 280 other module bodies and all 21 Print
modules remain unchanged.

Exact immutable golden:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintRocketHostMemoryOutputBufferCompare.py` checks `AXI4Buffer`'s five
queue instances and `Queue_22/23/24/26`'s payload ABI, two-entry geometry,
handshake/RAM equations and sequential reset/pointer/occupancy updates.
Payload widths are 64/73/7/72 bits for address/write/ack/read. The candidate's
five helper definitions also match the native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/
post-fame-host-memory-output-buffer.mlir` exactly.

After sourcing the project environment, replay both iteration-110 Print
R-response boundaries with `goldengate-print-binding-test
--rocket-host-memory-output-buffer-boundary INPUT OUTPUT_PREFIX` and its
`-reverse` variant. Each checks 24 malformed inputs plus repeated attachment,
including all helper collisions, missing channels, wrong constructors/ports,
extra host uses/instances and missing/duplicate/conflicting clock/reset
bindings. Run `goldengate-host-memory-output-buffer-test
--expanded-boundary OUTPUT` to interpret the candidate's emitted queue SSA:
256 exhaustive state/handshake/reset cases, followed by 6,000 five-channel
cycles with independent stalls and resets. Run the structured comparator
with paired `--before`/`--after`, `--baseline`, `--sfc`, and `--report-json`.
Mutable evidence is under
`iteration111-print-rocket-host-memory-output-buffer/` in the generated tree.

Next smallest step: compose the native `specializeHostMemoryPort` on this
expanded boundary, compare the 34/64/16-bit `mem_0` interface and request ID
zero extension/response ID trimming against SFC FPGATop. Production Print
CLI/driver integration/runtime parity, general FAME1/FAME5, multiclock and
the SFC UART-bearing baseline remain incomplete. Supplied iteration-110
harness feedback passes CIRCT replacertl and required Verilator workloads;
those gates do not establish runtime parity for this expanded Print change.
No FireSim manager verification step was started by the implementation agent.
Shared clock-control, recovery and harness edits remain outside this checkpoint.

Validation: the compiler and Print/native host response/output-buffer test
executables build. All three focused CTests pass in 12.99 seconds. Both
expanded replays and all 25 atomic rejections per order pass. Each emitted
candidate passes 256 exhaustive queue states (48 reset writes, 64 full states,
32 read/write collisions) and 6,000 hierarchical five-channel SSA edges:
17,380 enqueues, 17,147 dequeues, 8,350 stalled outputs, 6,765 full states,
9,487 simultaneous transfers and 44 resets. The structured comparison passes
136 SFC queue equations, 36 payload ABI checks, 12 sequential state-update
checks and the five AXI4Buffer instance mappings. All 280 prior non-top bodies,
109 top ports, 120 routes, five native helper definitions, 21 Print modules
and the recursive archive match in both orders. FIRRTL verification passes;
all six replay/semantic/comparison/CTest stderr logs are empty. Overall port
completion is not claimed.

### Expanded Print host memory platform port (iteration 112)

`mapPrintBridgeRocketHostMemoryPort` composes the native
`specializeHostMemoryPort` after the five-channel host output buffer.
The Scala oracle is `MidasTransforms`'s post-SRAM `SimulationMapping` /
`HostSpecialization` boundary and `FPGATop.scala`'s `mem IO <> memAXI4Nodes`
connection. The new `GGHostMemoryPlatformWrapper` exposes a 34-bit address,
64-bit data, 16-bit ID `mem_0` interface. Its combinational
`GGHostMemoryPortAdapter` uses FIRRTL `pad` for AW/AR request IDs and `bits`
for B/R response IDs; other payload/handshake leaves pass through. Conversion
does not depend on valid or ready and introduces no state.

The Print entry point validates the live expanded allocation, stages the shared
native pass, verifies FIRRTL and transfers only the two new module definitions.
All 286 prior bodies, live operations, arguments and port metadata remain
intact. The top has 109 ports (108 copied plus `mem_0`), two instances and 110
connections. Recursive copied-port targets transfer to the new wrapper;
consumed `host_mem` references retain their original module/port identity under
the new circuit name. All 21 Print modules retain their prior bodies.

The native preflight now counts both ordinary and strict clock/reset connections
and requires exactly one driver per buffer input. This accepts either unique
connect form and rejects duplicate or conflicting drivers across both forms.

Exact immutable golden:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintRocketHostMemoryPortCompare.py` compares FPGATop's complete 37-leaf
`mem_0` ABI and every leaf equation, including the two five-to-sixteen-bit
zero extensions and the two sixteen-to-five-bit truncations. It checks the
candidate's copied ports, 110 routes, prior module bodies and recursive targets
in both constructor orders. The helper also matches the native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/
post-fame-host-memory-platform.mlir` exactly.

After sourcing the project environment, replay both iteration-111 expanded Print
output-buffer boundaries with `goldengate-print-binding-test
--rocket-host-memory-port-boundary INPUT OUTPUT_PREFIX` and its `-reverse`
variant. Each exercises 25 malformed inputs plus repeated attachment: helper
collisions, missing archive/allocation, wrong constructor/ports, extra uses or
instances, and missing/duplicate/conflicting clock/reset/output connections.
`goldengate-host-memory-port-test --expanded-boundary OUTPUT` interprets the
emitted adapter SSA across all 65,536 response IDs, all 32 request IDs and
all four valid/ready combinations, with randomized non-ID payloads. Each order
has 262,144 cases and 9,699,328 leaf checks. The native CTest also covers all
ordinary/strict clock/reset combinations. Mutable evidence is under
`iteration112-print-rocket-host-memory-port/` in the generated tree.

Next smallest step: compose `assembleCPUStreamPort` on this expanded boundary,
resolving its native-only `GGEmptyCPUStreamWrite` assumption against the
preserved `CatalogRocket_GGEmptyCPUStreamWrite` and the live TracerV/Print stream
bindings, then compare the platform stream ABI and routes against SFC.
Production Print CLI/driver/runtime parity, general FAME1/FAME5, multiclock and
the SFC UART-bearing baseline remain incomplete. Supplied iteration-111
harness feedback passes CIRCT replacertl and required Verilator workloads;
those gates do not establish runtime parity for this expanded Print change.
No manager verification step was started. Shared clock-control, recovery and
harness changes remain outside this checkpoint.

Validation: the compiler and all three focused test executables build. The
Print binding, native memory-port and output-buffer CTests pass in 59.25 seconds.
Both expanded replays pass all 26 atomic rejections and FIRRTL verification.
Each emitted adapter passes all 262,144 cases / 9,699,328 leaf checks. The
structured SFC ABI/equation, native-helper, prior-body, copied-port, route and
recursive-target comparisons pass in both orders. All five replay, behavior
and comparison stderr logs are empty. Overall port completion is not claimed.

### Expanded Print empty CPU write attachment (iteration 113)

Replaying `assembleCPUStreamPort` against both iteration-112 Print boundaries
rejected the candidate because the live top has only twelve AR/R scalar fields;
the 25-field buffered CPU interface is not yet present. The archived empty
writer and CPU queues are definitions, not live expanded connections. This
observed mismatch selects the smaller engine-facing empty-write attachment.
The rejected aggregate attempts remain in the mutable iteration-113 evidence.

`mapPrintBridgeRocketEmptyCPUStreamWrite` now stages the shared native
`attachEmptyCPUStreamWrite` on the validated expanded allocation. It connects
the preserved `CatalogRocket_GGEmptyCPUStreamWrite` to host clock/reset and
adds eleven typed AW/W/B scalar fields in `GGPrintCPUStreamWriteWrapper`.
The wrapper has 120 ports, two instances and 122 connections. All 288 prior
module bodies, operation objects, SSA arguments and port metadata remain
unchanged, including all 21 Print modules. Recursive copied-port targets move
to the new top; annotation classes and counts remain intact. Every rejection
precedes mutation; the Print entry point verifies staged FIRRTL before transfer.

The Scala oracle is `CPUManagedStreamEngine.scala`'s empty `sinkParams`, default
write handshakes/response and its AW-size/W-strobe assertions, together with
`FPGATop.scala`'s CPU-managed AXI buffer boundary. `MidasTransforms.scala` places
host specialization after FAME and simulation mapping; `FAMETransform.scala`
provides the host clock/reset context. This attachment reuses the native
FIRRTL engine operations: AW/W ready, B valid and B response are zero, B ID
tracks the live AW ID even when B is invalid, and the clocked size-6/all-strobe
assertions are disabled during host reset. External queue behavior remains a
separate step: the full SFC platform interface can buffer writes before stalling.

Exact immutable golden:
`/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintRocketEmptyCPUStreamWriteCompare.py` matches `CPUManagedStreamEngine`'s
live-ID equation and both reset-gated assertion predicates, `AXI4Buffer_2`'s
zero AW dequeue ready, the eliminated W dequeue-ready port in `Queue_52`, and
`Queue_53`'s disabled B enqueue/default response. It separately checks every
wrapper route/port, all prior bodies and recursive targets in both constructor
orders. The reused helper matches the native iteration-81
`candidate/post-fame-cpu-stream-platform.mlir` exactly.

Native CPU port assembly also identifies the writer through its zero-sink
stream contract and requires exactly one matching ordinary or strict forwarding
driver. Tests cover a catalog-renamed writer, strict forwarding, duplicate
contracts and duplicate/conflicting drivers without changing rejected IR.

Validation: the compiler and three focused test executables build; Print
binding, CPU stream port and empty CPU stream write CTests pass (6.62 seconds).
Both `--rocket-empty-cpu-stream-write-boundary` replays pass 13 atomic rejection
checks and FIRRTL verification. Each emitted helper passes 73,984 interpreted
operation cases, covering all 65,536 live AW IDs, eight sizes, reset/valid/ready
combinations and all 64 missing-strobe lanes. The updated native CPU port pass
also replays the real Rocket boundary: 89 ports and 1,676 retained annotations. The structured SFC comparison
passes in both orders: 288 unchanged bodies, 120 ports, 122 routes and recursive
target transfer. Replay, behavior and comparison stderr logs are empty.
Evidence is under `iteration113-print-rocket-cpu-stream-port/` in the generated
tree; `empty-write-comparison.json` records the golden and both candidates.

Supplied iteration-112 harness results pass CIRCT replacertl and all required
Verilator workloads. The complete buildbitstream stdout/stderr and manager log
were inspected: Vivado 2023.2.2 fails before synthesis because
`bd_lib/2023.2.2/create_aurora_exdes.tcl` is absent. No manager gate or direct SSH
was started. Shared platform, clock-control and harness changes are preserved.

Next smallest step: compose the native depth-two AW/W CPU queues on this
expanded empty-write boundary, including AW address/length, and compare their
stall/reset behavior with SFC before composing AR/R/B queues and the aggregate
CPU platform port. Production Print CLI/driver/runtime parity, general
FAME1/FAME5, multiclock and the SFC UART-bearing baseline remain incomplete.
Overall port completion is not claimed.

### Iteration 114: expanded Print CPU AW/W buffering

`mapPrintBridgeRocketCPUStreamWriteBuffer` now stages the shared native
`attachCPUStreamWriteBuffer` builder after the expanded empty CPU writer. It
publishes `GGPrintCPUStreamWriteBufferWrapper`, `GGCPUStreamAWQueue2`, and
`GGCPUStreamWQueue2`; no retained module body, port metadata, SSA argument or
operation is replaced. The native production entry point delegates to the same
builder with its existing boundary names.

The Scala oracle is `FPGATop.scala`'s `AXI4Buffer()` between the CPU master and
stream engine, in the post-FAME `SimulationMapping` neighborhood recorded by
`MidasTransforms.scala`. Both AW/W queues have depth two, flow=false and
pipe=false. RAM read latency is zero and write latency one. Reset flushes the
three control bits, while a pre-edge accepted enqueue still writes RAM during
reset. The empty incoming stream stalls each dequeue independently: two
transactions can enter each queue before its ready drops. The AW ID and W strb
presented to the engine then remain at the first accepted payload until reset.
AW addr/len are stored for the shared Queue_51 shape, though this empty writer
has no consumers for them. W data/last remain absent, consistent with SFC's
eliminated fields.

Exact immutable golden compared:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketCPUStreamWriteBufferCompare.py` checks `Queue_51`, `Queue_52`, and
`AXI4Buffer_2`: payload widths, depth-two memories, pointer/occupancy equations,
reset-only control flushing, accepted reset-edge RAM writes and independent
channel wiring. SFC removes W's constant-zero dequeue ready and pointer;
CIRCT retains the generic native queue and connects its dequeue to the native
zero-ready engine. The generated queue bodies match the shared native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/post-fame-cpu-stream-platform.mlir`.

Both constructor orders preserve 289 prior module bodies and all 21 Print
modules, produce 292 modules and 122 top ports (120 plus AW addr/len), and have
133 wrapper routes. Seven engine-facing AW/W targets remain on the consumed
inner boundary; 113 copied ports, including host clock/reset, mem_0, B and AR/R,
move to the new boundary. A nested mixed-target probe and the complete retained
annotation archive pass comparison. Each replay passes 11 atomic rejection
cases, including collisions, absent archive/allocation, malformed clock/write
fields, already-instantiated boundaries and repeated application.

`CPUStreamWriteBufferTest.cpp` now accepts an actual generated boundary and
interprets its native queue operations against an independent deque. Each
order passes 20,400 cycles per queue: stalled fill/head retention, random
occupancy, full/empty no-bypass behavior, simultaneous enqueue/dequeue,
pointer wrap, and reset with accepted RAM writes. Across both orders this is
81,600 checked cycles. Focused CTests `goldengate-print-binding`,
`goldengate-cpu-stream-write-buffer`, and `goldengate-cpu-stream-read-buffer`
pass. Artifacts, stdout/stderr and comparison JSON are under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration114-print-rocket-cpu-aw-w-buffer/
```

Next: attach the shared native AR queue to this expanded CPU boundary, then
R and B queues and the platform aggregate. Production Print integration,
enabled Print runtime equivalence, and the UART-bearing SFC runtime baseline
remain pending. No manager verification steps were started in this iteration.


### Iteration 115: shared CPU AR queue on the expanded Print boundary

`attachCPUStreamReadBuffer` now implements both the original CPU transport
boundary and the expanded Print boundary. The native production entry point
still uses its existing names. `mapPrintBridgeRocketCPUStreamReadBuffer`
validates the Print/FASED allocation, stages the shared builder, verifies the
result and publishes only `GGPrintCPUStreamReadBufferWrapper`. Its independent
`arQueue` instance reuses `GGCPUStreamAWQueue2`, as SFC's AW and AR instances
reuse Queue_51. No previous module, operation, SSA argument, port annotation
or inner symbol is replaced.

The Scala oracle is `FPGATop.scala`'s default `AXI4Buffer()` attachment in the
post-FAME `SimulationMapping` neighborhood of `MidasTransforms.scala`.
Exact immutable golden compared:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketCPUStreamReadBufferCompare.py` checks Queue_51's four payload
fields (id16/addr64/len8/size3), depth-two memories, non-flowing/non-pipelined
occupancy, reset control flushing and accepted reset-edge RAM writes. It
checks all 14 AR attachment routes in AXI4Buffer_2 and the golden engine's
`grant & R_ready & stream_valid & lastReadBeat` AR dequeue condition, with
R id taken from the retained AR. The queue body matches the shared native
baseline `iteration81-print-rocket-fased-read-buffer/candidate/post-fame-cpu-stream-platform.mlir`.

Both constructor orders preserve all 292 prior module bodies and all 21 Print
modules, produce 293 modules, retain 122 public ports and have 130 wrapper
routes. Six consumed AR targets stay on the inner boundary; 116 copied ports,
including host clock/reset, AW/W/B, R and mem_0, move to the new boundary.
The nested mixed-target probe and complete retained annotation archive pass
comparison. Each replay passes 15 atomic rejection cases, covering collisions,
missing archive/allocation/helper, wrong active boundary, malformed clock/AR
ports, unsupported AR fields, already-instantiated tops, invalid queue
attributes/interface/memory latency and repeated application.

`CPUStreamReadBufferTest.cpp` accepts the actual generated boundary and
interprets its queue operations against an independent deque. Each order
passes 21,450 cycles, including two pending requests held through eight
non-final/stalled response beats each, full-queue pressure, pointer wrap,
randomized enqueue/dequeue, full/empty no-bypass, simultaneous transfers,
reset flush and accepted reset-edge RAM writes. Across both orders this is
42,900 checked cycles. Focused CTests for Print binding, CPU AW/W buffering,
CPU AR buffering and the single/multistream CPU read engines pass; the
production compiler builds.
Artifacts, stdout/stderr and comparison JSON are under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration115-print-rocket-cpu-ar-buffer/
```

Next: attach the native R response queue to this expanded boundary, then B
and the platform aggregate. Production Print integration, enabled Print
runtime equivalence, and the UART-bearing SFC runtime baseline remain pending.
No manager verification steps were started in this iteration.


## Iteration 116: expanded Print CPU R response buffering

`attachCPUStreamResponseBuffer` shares the native depth-two R queue builder
between the existing simulator path and the expanded Print circuit.
`mapPrintBridgeRocketCPUStreamResponseBuffer` requires the validated expanded
CPU AR boundary, clones for staging, verifies the result, and publishes only
`GGCPUStreamRQueue2` and `GGPrintCPUStreamResponseBufferWrapper`. Existing
modules, operations, SSA arguments, port annotations and inner symbols retain
identity. Six R targets stay on the inner boundary; the remaining 116 ports
move to the new outer boundary in the recursively transferred archive.

The Scala oracle is `FPGATop.scala`'s default `AXI4Buffer()` in the
`SimulationMapping` neighborhood of `MidasTransforms.scala`; its host clock
and reset follow the conventions established by `FAMETransform.scala`.
Exact immutable golden compared:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketCPUStreamResponseBufferCompare.py` checks Queue_55's
id16/data512/resp2/last1 RAM payloads, depth two, flow=false, pipe=false,
occupancy and reset equations, asynchronous reads and accepted reset-edge
RAM writes. SFC folds the engine's constant Okay response into the queue's
RAM write instead of exposing an engine response port. The comparison checks
that specialization and the native engine's constant-zero response, all SFC
AXI4Buffer_2 R routes, and all 130 native wrapper routes. The queue body is
identical to the shared native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/post-fame-cpu-stream-platform.mlir`.

Both annotation orders retain all 293 previous module bodies and all 21 Print
modules, produce 295 modules, and retain all 122 public ports. Mixed nested
copied/consumed targets and the complete retained annotation archive match.
Each order checks 11 atomic rejections for collisions, missing archive or
Print allocation, wrong boundary, malformed clock/R interface, unsupported
R fields, instantiated top, wrong payload width and repeated application.

`CPUStreamResponseBufferTest.cpp` accepts the actual emitted boundary and
interprets its queue operations against an independent deque. Each order
checks 20,400 cycles using all 512 data bits and all four response fields,
including full/empty no-bypass, pointer wrap, stalls, simultaneous transfers,
reset flush and reset-edge RAM writes: 40,800 cycles across both orders.
Focused Print binding, CPU AR and CPU R CTests pass; the production compiler
builds. Artifacts and stdout/stderr are retained under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration116-print-rocket-cpu-r-buffer/
```

Next: attach native B response buffering to this expanded boundary, then the
platform aggregate. Production Print integration, enabled Print runtime
parity, and the UART-bearing SFC runtime baseline remain pending. Manager
verification remains owned by the harness; none was started here.

### Iteration 117: expanded Rocket Print CPU B response buffer

`mapPrintBridgeRocketCPUStreamWriteResponseBuffer` extends the verified
`GGPrintCPUStreamResponseBufferWrapper` boundary with
`GGPrintCPUStreamWriteResponseBufferWrapper` and `GGCPUStreamBQueue2`.
`attachCPUStreamWriteResponseBuffer` shares the native FIRRTL queue builder
with the production zero-Print path. The mapper validates live Print
allocation, builds and verifies a clone, then publishes only the two new
modules and the circuit/annotation rename. Existing module, port, SSA and
operation identities remain intact; rejected inputs leave the circuit intact.

`PrintRocketCPUStreamWriteResponseBufferCompare.py` compares both annotation
orders against these modules in the exact immutable golden artifact:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The comparison passes for `Queue_53`, `AXI4Buffer_2` and
`CPUManagedStreamEngine`: id16/resp2 RAM payloads, depth two, asynchronous
reads, reset of dequeue control, and all seven specialized B routes. SFC
specializes enqueue valid to zero for Rocket's empty sink list, removing
enqueue/full state and disabling RAM writes. It retains RAM payload reads
while invalid; the live engine AW ID and constant Okay response must not
bypass these reads. The native helper retains generic queue state until
standard CIRCT lowering. Its body matches the shared native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/post-fame-cpu-stream-platform.mlir`.

Each order produces 297 modules while retaining all 295 previous module
bodies and all 21 Print modules. All 122 public ports remain: 118 copied and
four consumed B ports. All 128 wrapper routes and the complete recursively
retargeted annotation archive match. Each order passes 11 atomic rejection
checks, covering helper collisions, absent archive/allocation, wrong top,
malformed clock/B interface, unsupported fields, instantiated top, wrong ID
width and repeated application.

The C++ interpreter exercises the actual emitted `GGCPUStreamBQueue2` in each
order against an independent deque for 20,400 active-traffic cycles, including
full/empty no-bypass, pointer wrap, stalls, simultaneous transfers, reset flush
and accepted reset-edge RAM writes. An additional 1,024 zero-sink cycles per
order check permanently empty occupancy, disabled writes and retained invalid
RAM reads. Both orders pass. Print binding, CPU R and CPU B CTests pass (3/3),
and the production compiler builds. Complete local stdout/stderr and the
structured comparison report are retained under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration117-print-rocket-cpu-b-buffer/
```

Next: assemble the expanded buffered CPU scalars into the native platform AXI
aggregate. Production Print integration and enabled Print runtime parity
remain pending, as does the UART-bearing SFC runtime baseline. Iteration 116
harness feedback reports passing CIRCT replacertl and required Verilator
Rocket suites; it does not validate this enabled Print boundary. Manager
verification remains owned by the harness; none was started here.

### Iteration 118: expanded Rocket Print CPU platform AXI slave

`mapPrintBridgeRocketCPUStreamPort` assembles the buffered 25 CPU scalars in
`GGPrintCPUStreamWriteResponseBufferWrapper` into the 64/512/16 AXI slave in
`GGPrintCPUStreamPlatformWrapper`. It validates live Print allocation, stages
`attachCPUStreamPort` on a clone, verifies the result, and publishes only the
new wrapper and shared `GGCPUStreamPortAdapter`. All prior module, port, SSA
and operation identities remain intact. Copied targets transfer recursively;
CPU scalar targets remain on the retained inner module.

The shared preflight supports the original memory platform boundary and the
expanded Print B boundary. It compares actual SSA roots and FIRRTL field IDs,
counts ordinary and strict drivers together, and requires unique forwarding.
The four buffered B leaves resolve to the retained queue's dequeue bundle;
the other CPU scalars resolve to the retained simulator child. No engine,
queue, state, protocol gating or target-clock behavior is changed.

The Scala oracle is `FPGATop.scala`'s flipped CPU AXI bundle and direct node
connection, following its `AXI4Buffer()` and the `SimulationMapping` boundary
in `MidasTransforms.scala`. Host clock/reset conventions follow
`FAMETransform.scala`. Exact immutable golden compared:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

`PrintRocketCPUStreamPortCompare.py` checks FPGATop's complete 37-leaf CPU
slave ABI, all 25 buffered identity routes, and the 12 unused metadata/data
inputs in the zero-sink profile. It checks all 123 native wrapper connections,
97 copied ports, the 25 consumed scalars, the 98 resulting public ports and
the complete recursive annotation archive. All 297 prior module bodies and
all 21 Print modules remain; the candidate contains 299 modules. The adapter
body also matches the shared native baseline
`iteration81-print-rocket-fased-read-buffer/candidate/post-fame-cpu-stream-platform.mlir`.

The production compiler and both focused test binaries build. Print binding
and CPU stream port CTests pass (2/2, 5.49 seconds). Both actual Rocket Print
orders pass 17 atomic rejection checks, the structured golden comparison,
and identity checks for every connected CPU payload bit. The original native
Rocket replay also passes with 89 ports and 1,676 retained annotations. All
replay, behavior and comparison stderr logs are empty. Evidence is retained
under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration118-print-rocket-cpu-platform/
```

Next: compose the expanded CPU/control/memory boundary into the native
`FPGATop` shell, then continue through platform specialization and collateral.
Production enabled-Print integration and runtime parity remain pending, as
does the SFC UART-bearing runtime baseline. Supplied iteration-117 harness
feedback passes CIRCT replacertl and required Verilator suites; these gates
do not validate the expanded enabled-Print boundary. No manager verification
step was started here. Shared recovery, clock-control and harness edits remain
outside this checkpoint.

## Expanded Print Rocket FPGATop shell (iteration 119)

`mapPrintBridgeRocketFPGATopShell` validates the live Rocket Print allocation
and composes the shared native shell around `GGPrintCPUStreamPlatformWrapper`.
The shell retains its entire inner boundary and exposes only `clock`, `reset`,
`ctrl`, `mem_0` and `cpu_managed_axi4`, with five direct aggregate connections.
It adds no state, buffering, clock gating or target-cycle logic. The expanded
profile permits passive data diagnostics to remain internal; hidden inputs,
flipped diagnostic leaves and unrecognized clocks are rejected. The original
profile retains its diagnostic name restrictions.

The shared shell now requires one exact forwarding use on both the parent
argument and child result, checks ordinary/strict drivers, and accepts valid
strict scalar forwarding. This rejects an alternate child driver that does
not increase the public argument's use count. FAME host clock/reset targets
must identify the active inputs exactly once. Public targets transfer to the
new shell (clock/reset renamed), private targets remain on the retained inner
module, and one `HostClockSource` is produced on `~FPGATop|FPGATop>clock`.
Clone staging verifies the result before publishing only the new shell, so all
existing module, port, SSA and operation identities survive and rejection is
atomic. Hierarchy analyses must be rebuilt after wrapping.

The oracle is `FPGATop.scala`'s host clock annotation and direct flipped
control/CPU and master memory IO, following `SimulationMapping` in
`MidasTransforms.scala` and the host conventions in `FAMETransform.scala`.
The expanded replay input comes from iteration 118 in both constructor orders.
Its synthetic enabled-Print handoff lacks FAME host identities: the test
explicitly supplies those two annotations and tests rejection without them.
The compiler does not fabricate them. This is a boundary test, not evidence
that enabled-Print production ingestion or runtime parity is complete.

Reproduce with `goldengate-print-binding-test
--rocket-fpga-top-shell-boundary[-reverse] INPUT OUTPUT_PREFIX`, then
`PrintRocketFPGATopShellCompare.py --before INPUT --after OUTPUT --sfc GOLDEN
--stub STUB --report-json REPORT`. Lower the emitted boundary-only stub with
pinned `firtool --format=mlir STUB -o RTL`, and repeat the comparison with
`--rtl RTL`. The stub contains the actual new shell and an opaque external
child declaration; it is not a replacement simulator candidate.

The exact immutable golden compared is:

```text
/scratch/jfx/fsim-circt/sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv
```

The emitted FPGATop RTL matches all 121 external leaf names, widths and
directions: clock/reset plus control (45), memory (37) and CPU (37). Both
expanded orders retain all 299 prior module bodies, all 21 Print modules and
93 private diagnostics, giving 300 modules with the new shell. The comparison
checks all five native aggregate routes and the complete recursive annotation
archive, including unique HostClockSource and private/public target probes.
Both orders pass 17 atomic rejection checks and preserve existing IR identities.
The shared shell test also covers valid strict scalar forwarding and competing
strict drivers in both boundary profiles. Local module printing avoids repeated
global printer-state construction during the large replay's body snapshots.

Production compiler and focused test binaries build. The two CTests pass
(2/2, 5.48 seconds). The original native Rocket replay produces five ports and
1,677 annotations with all prior module bodies unchanged. Successful replay,
comparison and lowering stderr logs are empty. Evidence and `compare.json`
are under:

```text
sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration119-print-rocket-fpga-top-shell/
```

Next: compose this verified expanded boundary through shared F1Shim platform
specialization, then collateral. Enabled-Print production integration and
runtime parity remain pending; the immutable SFC fixture has Print disabled.
Supplied iteration-118 CIRCT replacertl/Verilator gates pass, with the SFC
UART-bearing runtime baseline still pending. Those gates do not validate this
expanded Print path. No FireSim manager verification step was started here.
Shared recovery, clock-control, harness and outer-plan changes are excluded
from this checkpoint.

### Expanded Print Rocket F1Shim specialization (iteration 120)

`mapPrintBridgeRocketF1Shim` composes the expanded Print FPGATop boundary
through the shared CIRCT U250 specialization. Before cloning, it requires one
expanded Print CPU platform instance and exactly five identity connections,
with matching names, types and directions. Missing, repeated, competing or
misdirected connections are rejected. The shared specializer requires the
exact one-channel U250 AXI contracts and a unique retained HostClockSource;
existing DDR defines/XDC paths and unsupported platform shapes are rejected.
After verification, only the new F1Shim module and circuit/archive identity
are published. Existing modules, ports, SSA arguments and operations survive.
Hierarchy analyses must be rebuilt after the new outer module is published.

The semantic oracle is `platform/F1Shim.scala`, including the two synchronous
host-reset 12-bit request ID counters that advance on independent control AW
and AR handshakes, the AXI4/Nasti PCIS conversion, inactive QSFP lanes, and DDR
channel defines. `PlatformShim.scala` supplies the U250 XDC paths. This stays
outside FAME target-cycle state and follows the platform boundary described
in `MidasTransforms.scala`; the FAME host convention comes from
`FAMETransform.scala`. The mapper consumes no annotations, recursively
retargets circuit identity while retaining local targets, and produces one DDR
OutputFile and one XDCPathToCircuitAnnotation.

Reproduce using `goldengate-print-binding-test --rocket-f1-shim-boundary[-reverse]
INPUT OUTPUT_PREFIX` with the iteration-119 expanded shell outputs. Compare
both orders using `PrintRocketF1ShimCompare.py --before INPUT --after OUTPUT
--baseline NATIVE_F1SHIM --sfc GOLDEN --stub STUB --report-json REPORT`.
Lower the actual new module with pinned `firtool --format=mlir STUB -o RTL`,
then compare again with `--rtl RTL`. The stub preserves the actual F1Shim and
an external FPGATop declaration; it is boundary evidence, not a full simulator.
`goldengate-f1-shim-test --emit-collateral OUTPUT STUB` emits collateral from
the verified expanded candidate and checks five invalid file sets leave the
output bytes untouched. Pass the resulting `.defines.vh` as `--defines`.

The comparison checks every retained module and the complete recursive raw
annotation archive, the shared F1Shim body, all external leaf names/widths/
directions and every combinational platform route against SFC. An interpreter
executes the candidate's actual register-driver CIRCT DAG over 30,000 host
cycles, independently stalling AW/AR, forcing counter wraps and synchronous
resets, and varying ignored incoming request IDs. The SFC always blocks and
expressions establish the reference transition. Four injected counter faults
(reset value, increment, rollover width and handshake qualification) must be
caught by the differential schedule.

The replay inherits the explicitly seeded FAME host identities from iteration
119. Neither mapper fabricates identities. The immutable SFC fixture has Print
disabled; its platform ABI and shared behavior are useful oracles, but matching
them does not establish enabled-Print production ingestion or runtime parity.
Next: compose expanded Print driver/collateral emission, then connect the
validated expanded path to production ingestion and its enabled-feature gate.

Iteration-120 validation passed: production compiler and both focused test
executables build; `goldengate-print-binding-test` and `goldengate-f1-shim-test`
CTest entries pass (2/2). Both real expanded Rocket replays retain all 300
prior module bodies, including 21 Print modules, preserve port/SSA/operation
identities, and pass 18 atomic rejection cases per order. The complete raw
archive transfer and the unique retained HostClockSource match. Pinned firtool
lowers the actual new F1Shim boundary successfully. The comparison matches
143 external leaf interfaces and 129 canonical combinational routes; the
counter DAG matches 30,000 host cycles, two wraps per counter, 31,508 independent
stall events and three reset cycles. All four counter mutants are rejected.
Candidate OutputFile emission matches the full DDR header; five invalid
collateral requests preserve existing output bytes and IR. U250 XDC paths
remain `firesim_top` before and after linking.

Exact immutable SFC artifacts compared (read only):

- `sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`
  — F1Shim interfaces, combinational routes and synchronous counter transitions
  match the candidate's corresponding platform output.
- The same directory's `FireSim-generated.defines.vh` — exact emitted-byte
  match for all DDR channel defines and comments.

Evidence is under the U250 generated-source directory's
`iteration120-print-rocket-f1-shim/`: `ctest-final.log`,
`replay-retry[-reverse].stdout.log`, `f1-shim.stub.mlir`, `f1-shim.sv`,
`FireSim-generated.defines.vh`, `f1-shim-all.json`, `collateral.stdout.log`,
`compare-retry.stdout.log`, and `compare.json`. Earlier failed local attempts
are retained separately: one test direction mutation initially selected the
unchanged input direction, and comparison assumptions required the MLIR
newline escape and externally declared child interface. Corrected replays
and comparisons above pass. No manager verification gate was started by this
iteration; the supplied prior gate passes do not validate enabled Print.


### Expanded Rocket Print driver allocation (iteration 121)

`preparePrintBridgeAllocatedHeader` now accepts the expanded Rocket F1Shim
boundary as well as the selected control master. It calls the shared Rocket
allocator over the actual original bridge, Print, LoadMem and CPU count
registries. This preserves FPGATop's registration order and stable region sort;
a Print-only allocation at this boundary would give incorrect driver addresses.
Every active decoder region must equal the derived complete map. Print/count
bindings, six-word configuration permissions, queued host identities and native
stream indices are validated before any constructor text is published.

The CPU header emitter now discovers the empty incoming writer by its contract
attribute and unique live InstanceGraph path. Imported catalog symbols retain a
prefix; their spelling does not determine whether the writer exists. Additional
live writer definitions or multiple live paths are rejected, including an extra
writer instantiated twice alongside the unique allocated writer. The existing
SSA checks still require count and data to resolve to the same live storage,
32-bit read-only count words, the actual DMA selector/dequeue handshake, queue
capacity and inactive incoming handshakes. No compiler IR, annotations or
existing collateral are changed by the Print fragment API, including rejection.

Reproduce with `goldengate-print-binding-test --rocket-allocated-header INPUT
OUTPUT_PREFIX`, using both iteration-120 `binding.mlir.rocket-f1-shim*.mlir`
outputs. The test emits `.const.h` and `.decoders.h` fragments, rejects nine
invalid boundaries atomically per order, and checks binding traversal order
independence. Allocation text agrees between orders. Decoder clock ratios
follow the respective instantiated host order (the reversed fixture intentionally
exchanges the two bridge clocks). Compile each pair under the standard GET_*
guards against the actual `widget_registry_t`, synthesized Print and CPU stream
headers. `PrintRocketAllocatedHeaderCompare.py INPUT HEADER GOLDEN REPORT`
compares the resulting addresses/descriptors with active CIRCT allocation and
the immutable SFC RTL. Production compiler and focused test builds succeed;
`goldengate-print-binding` and `goldengate-cpu-managed-stream-header` pass (2/2),
including 66 CPU header rejection cases. Both constructor sets compile with
`-std=c++17 -Wall -Wextra -Werror -fsyntax-only`.

Exact read-only golden artifact:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
Eight native bank regions remain identical. The shared CPU transport ABI matches:
6144 512-bit beats, 524288-byte DMA windows, padded 32-bit read-only counts and
empty incoming writer behavior. The fixture disables Print; candidate Print
bases are 544 and 576, CPU count addresses are 624/628/632, versus the oracle's
single TracerV count at 568. This expected catalog difference is recorded rather
than reported as absolute-address parity.

Evidence is in the U250 generated-source directory's
`iteration121-print-rocket-header/`: replay stdout/stderr for both orders,
constructor/decoder fragments, `compare.json`, C++ compilation logs and
`ctest.log`. Earlier local build/test-fixture mistakes and comparison assumptions
were corrected before the passing results; no immutable fixture was modified.
No FireSim manager verification gate was started. This is an expanded
Print/count fragment, not the complete enabled-Print production driver header.
Next: compose it with native Rocket constructors and interface/collateral output,
then connect the expanded path to production enabled-Print ingestion. Enabled
Print runtime and the SFC UART-bearing workload differential remain pending.

### Native Rocket header transaction and expanded TracerV (iteration 122)

`prepareRocketDriverHeaders` now composes all eleven native Rocket constructor
sections on a private CIRCT circuit clone. Production `goldengate.cpp` invokes
this API after interface generation, including its AutoILA capture path. Only
the retained output annotations are published after every emitter succeeds.
The original hardware, semantic/opaque archive and interface header survive an
early failure or a failure in the final FASED emitter. Constructor order follows
the existing production sequence and the driver's GET_CORE_CONSTRUCTOR,
GET_MANAGED_STREAM_CONSTRUCTOR and GET_BRIDGE_CONSTRUCTOR phases.

The TracerV header emitter now reads the actual ordered CPU stream catalog.
It checks the TracerV slot-zero identity and helper port layout, locates AXI
address/data ports after all source ports, and requires exactly one native SSA
address-window grant per allocated stream. Duplicate, missing and out-of-range
selectors are rejected. This permits the already-transformed three-stream
TracerV/Print boundary without weakening its queue/clock/MMIO checks.

Reproduce complete native collateral with `goldengate-print-binding-test
--rocket-native-driver-headers NATIVE_IR DIRECTORY`, using
`circt-ingestion/post-fame-f1-shim.mlir`. The replay regenerates its already
present interface outputs, retaining platform defines and all other annotations,
then tests eight atomic composition rejections, including the final FASED
failure, before emitting native `.const.h`, `.const.vh` and `.defines.vh`.
`PrintRocketDriverHeaderCompare.py NATIVE_IR DIRECTORY GOLDEN REPORT` checks
the constructor catalog, active allocated register addresses and the oracle's
F1Shim bus widths and NastiRouter regions.

Reproduce expanded focused collateral with `goldengate-print-binding-test
--rocket-driver-headers EXPANDED_IR DIRECTORY NATIVE_IR`, using both
iteration-120 F1Shim orders. This explicitly imports retained TracerV BridgeIO
and ChannelClockInfo analysis from the native boundary into the TEST fixture;
unseeded TracerV emission must reject. It emits `tracerv-print.const.h` and
`print-bridge-decoders.h`, checks four atomic catalog/selector rejections and
preserves hardware. These files contain focused TracerV/Print/CPU fragments,
not a complete enabled-Print production driver.

Complete expanded composition currently rejects the LoadMem boundary atomically:
its shared engine instantiates catalog-prefixed writer/FIFO helpers directly,
while the native header expects older live wrappers. The imported FIFO/writer,
expanded `FPGATop.mem_0`, immutable SFC `LoadMemWriter` and `F1Shim.io_slave_0`
all use 64-bit memory data; the separate CPU transport uses 512-bit data.
Resolve the expanded LoadMem live hierarchy and direct engine wiring contract
before publishing complete enabled-Print collateral; do not hide that mismatch
by renaming imported definitions or emitting unchecked constructors.

Exact read-only oracle remains the U250 fixture's
`design/FireSim-generated.sv` path recorded above. Evidence is in the generated
U250 directory's `iteration122-print-rocket-collateral/`, including complete
native and focused expanded headers, replay logs and structured comparisons.
Production/focused builds pass, as do four focused CTests and all three boundary
replays. All three constructor headers compile against the actual driver headers
with `-std=c++20 -Wall -Wextra -Werror -fsyntax-only` under the standard GET_*
phases. Native `.const.h` and `.const.vh` equal the preceding production output;
all eleven native regions and nine AXI widths match SFC. Both expanded headers
have identical allocation and the native TracerV constructor ABI. Their eight
unchanged large-bank regions and queue/count/DMA ABI match SFC, with the expected
new Print bases 544/576 and CPU counts 624/628/632 versus SFC's single count at
568. Initial test-fixture/nullable-attribute mistakes were corrected before these
passing replays; no immutable artifact was modified.
No FireSim manager verification gate was started. Enabled-Print production
ingestion/runtime and the SFC UART-bearing differential remain pending.

### Complete expanded Rocket/Print driver transaction (iteration 123)

`prepareLoadMemHeader` now resolves the direct expanded LoadMem engine's live
writer, packing FIFO, read request queue, unpacking FIFO and register bank by
instance identity. Catalog prefixes remain intact. The emitter validates the
nine public decoded MCR slots, both directions of the bank/vector connections,
byte strobes, shared host clock/reset, writer completion, data handshakes,
read-address acknowledgement and physical AXI connections through FIRRTL SSA.
It rejects conflicting declared/SSA MCR types and eight-word vectors before
publishing collateral. Native wrapper validation remains in place.

`prepareRocketDriverHeaders` now composes the expanded Print factories and CPU
stream descriptors with all eleven native constructors on the same private
circuit clone. Inline Print decoders are embedded in the ordinary `.const.h`
under GET_INCLUDES and a decoder include guard; a separate decoder file and
driver build dependency are unnecessary. The original circuit's annotations
are published only after the final FASED emitter succeeds. Print host, clock,
stream and register failures must leave hardware, the semantic/opaque archive
and the original interface header unchanged.

Reproduce native and complete expanded outputs with the iteration-122 replay
commands above. The expanded command now emits complete
`FireSim-generated.const.h`, `.const.vh` and `.defines.vh`; it replaces the
earlier focused TracerV/Print fragment output. The replay still explicitly
imports native TracerV BridgeIO/ChannelClockInfo into the TEST fixture and
requires unseeded TracerV emission to reject. This import is not a production
replacement for clock analysis.

`PrintRocketLoadMemHeaderCompare.py EXPANDED_IR HEADER NATIVE_IR NATIVE_HEADER
GOLDEN REPORT` compares actual direct engine connections and five live helper
bodies, all nine LoadMem addresses, the 64/32-bit chunk ABI, complete constructor
presence, native struct allocations and interface bus widths.
`PrintRocketAllocatedHeaderCompare.py` additionally checks both Print banks,
three CPU stream descriptors, the unchanged large-bank prefix and the SFC
queue/count/DMA contract.

Exact read-only oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
The relevant boundaries are LoadMemWidget, LoadMemWriter, NastiRouter,
Nasti2AXI4IdentityModule, Arbiter, Queue_27, CPUManagedStreamEngine and F1Shim.
The oracle disables Print: new Print bases 544/576, CPU counts 624/628/632 and
the shifted small-bank tail are expected catalog differences; LoadMem remains
at 384 with 64 bytes reserved and two 32-bit data chunks per memory beat.

Validation: production compiler and both focused binaries build. Five focused
CTest cases pass. Native replay passes eight atomic transaction rejections;
each expanded ordering passes eleven direct LoadMem, four TracerV and seven
complete transaction rejections, including a failure in the final FASED emitter.
An initial malformed MCR-type negative exposed a missing declared/SSA type
check; the corrected emitter rejects both inconsistent types and a consistently
typed eight-word vector. Final replay and comparison logs pass.

Both expanded headers match the SFC LoadMem boundary's sixteen memory ports,
75 widget equations, sixteen identity conversion routes, depth-two request
queue and queued-write priority over zero fill. All 80 direct CIRCT engine
connections and five unchanged live helper bodies agree with the native/SFC
contract. The nine addresses are 384 through 416 in four-byte steps. Native
comparison matches eleven regions and nine bus widths; expanded comparison
matches the eight unchanged regions and records the expected Print/count
allocation differences. LoadMem and TracerV constructor text is identical to
native in both orderings. Native `.const.h`, `.const.vh` and `.defines.vh`
remain byte-identical to iteration 122. All three complete headers compile
against actual driver includes with `-std=c++20 -Wall -Wextra -Werror
-fsyntax-only` under the standard GET_* phases.

Evidence: generated U250 directory `iteration123-print-rocket-collateral/`,
containing native/forward/reverse complete collateral, replay and C++ compile
logs, native comparison JSON, and both LoadMem/allocation comparison JSONs;
compiler build log is `iteration123-final-build.log`. No immutable fixture was
modified and no manager verification gate was started.

Next smallest step: connect production enabled-Print ingestion to the expanded
assembly, preserving actual retained TracerV/Print clock analysis through that
path, then invoke this complete transactional header API. Enabled-Print runtime
and the SFC UART-bearing differential remain pending; this milestone completes
the expanded boundary's driver collateral, not the overall compiler port.

### Checked bridge clock analysis transfer (iteration 124)

`BridgeClockInfo` now resolves retained BridgeIO clock identity to a CIRCT
module port and checks the sole ChannelClockInfo map, positive rational ratio,
and every unique mapped global channel. Production TracerV header emission
uses this shared analysis and additionally rejects two payload fields aliased
to one global channel. This alias previously passed when the clock records
matched. Existing twelve-channel, constructor, live graph and stream checks
remain in place.

`importBridgeClockInfo` transfers the analyzed metadata for one imported bridge
between assembly circuits in the same MLIR context. It resolves the source and
destination port, checks module/field/type/direction and declared versus SSA
port types, verifies both constructor identities, and requires one live
instance path in the destination. It copies only that bridge's analyzed channel
entries, merges them with existing destination domains, and retargets only the
BridgeIO circuit component. Conflicting clock entries and repeated imports
reject before mutation. Source hardware/annotations and unrelated destination
annotations, including opaque historical targets, remain intact. No clock is
inferred from the host or substituted when analysis is absent.

The expanded driver replay now invokes this compiler operation instead of
concatenating the complete native clock map and TracerV BridgeIO unchecked.
These historical expanded fixtures still need the native boundary as a source:
this does not connect enabled-Print production ingestion to the full assembly.
A small two-circuit FIRRTL fixture tests twelve atomic rejections, preservation
of an existing destination Print-domain entry, a selected-channel merge,
source immutability and a repeated-import rejection. The production TracerV
header test now includes the duplicate-channel case (35 total rejections).

Build and evidence: generated U250 directory `iteration124-build-check.log`
and `iteration124-bridge-clocks/`. The production compiler and both focused
binaries build; four CTests (Print binding, TracerV header, LoadMem header,
metasim interface header) pass. Native and both expanded complete-header
replays pass their existing hardware/archive and transaction checks. All nine
`.const.h`, `.const.vh`, `.defines.vh` outputs equal iteration 123 byte for byte;
all three full C++ headers compile against the actual drivers with
`-std=c++20 -Wall -Wextra -Werror -fsyntax-only`.

Exact read-only annotation oracle:
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json`.
Both emitted `bridge-clock-analysis.json` archives match its TracerV constructor
and twelve local channel identities; the imported per-channel clock records
match its RationalClockBridge constructor's named 1/1 domain. Their targets
resolve to `GGTracerVTokenEngine>hPort` in the destination assembly circuit.
`clock-comparison.json` records the comparison. Only the twelve selected
TracerV entries are imported, rather than the native archive's unrelated map
entries. The separate merge test establishes preservation of preexisting
clock-map entries; these old expanded fixtures have no preexisting clock map.

Exact RTL oracle remains the U250 fixture's
`design/FireSim-generated.sv` path recorded in iteration 123. Native comparison
matches all eleven regions and nine bus widths. Both LoadMem comparisons retain
its sixteen memory ports, 75 equations, sixteen identity routes, depth-two
request queue, write-over-zero-fill priority, nine MMIO addresses and 64/32-bit
chunk ABI. No immutable fixture was edited and no manager gate was started.

Next smallest step: invoke the checked transfer at the native bridge catalog
import in the enabled-Print production assembly, retain the Print synthesis
clock map through FAME/host binding, then call the complete driver transaction.
Enabled-Print production runtime and the SFC UART-bearing differential remain
pending; the overall compiler migration is incomplete.

### Production native control catalog ingestion (iteration 125)

`ControlCatalog` ports the host-definition linking part of the Scala
`SimulationMapping`/`PreLinkRenaming` boundary into CIRCT FIRRTL operations.
The generic import checks explicit roots, their complete instance dependency
closures, shared definitions, recursion and symbol collisions. A private
verified candidate is committed by moving only new definitions, preserving
the active top and existing operation handles. Private helpers receive the
`CatalogRocket_` namespace; compatible physical blackbox declarations are
reused even when their CIRCT symbols differ. Unrelated parameterized external
declarations remain intact. Source hardware and annotations never change;
failure leaves the destination intact.

The production `--compile-baseline --stop-after-print-host-binding` path now
accepts `--print-rocket-control-catalog NATIVE_F1SHIM.mlir`. It imports 21 public
Rocket register-bank/adapter roots and six private LoadMem engine roots. It
does not import the native platform top, allocated CPU transport or LoadMem
construction wrappers. The existing expanded assembly replay uses this same
compiler API instead of its unchecked test-only definition copier.

Production replay initially exposed a duplicate physical clock-gate declaration
and target hierarchy captured through LoadMem wrappers. The final import
reuses the compatible clock gate and roots the direct engines. Expanded replay
then found that MMIO allocation consumed the wrappers' register descriptors.
The importer validates the complete nine-word LoadMem registry/MCR interface
and retains those three descriptors on the direct engines. Allocation selects
those modules in linked catalogs and the original wrappers in native assembly.
Hardware operations remain identical to native; no register schema is invented.
The existing LoadMem comparison now checks every prior module body by identity
instead of assuming the historical 270-module catalog with duplicated targets.

Evidence is under the mutable generated U250 directory
`iteration125-control-catalog/`: build and CTest logs, `catalog.mlir`, production
enabled-Print ingestion MLIR/RTL and retained annotations, comparison reports,
CLI rejection report and production preservation report. Four focused CTests
pass. The small two-circuit fixture checks seven atomic rejection cases and
shared storage/physical blackbox identity. Native catalog replay additionally
checks three atomic malformed LoadMem descriptor rejections and the direct
nine-word MCR interface. Three invalid CLI option combinations reject early.
The focused expanded replay passes thirteen-bank allocation in both constructor
orders and 40 atomic rejection cases. Reproduce with
`goldengate-print-binding-test --rocket-catalog-allocation
circt-ingestion/post-fame-tracerv-stream-queue.mlir
circt-ingestion/post-fame-f1-shim.mlir OUTPUT_PREFIX`.
The historical forward/reverse LoadMem engine comparison also passes with
the updated body-preservation check. The longer end-to-end expanded attachment
replay was stopped after progressing through FASED attachment; it is not
reported as a complete replay pass.

Exact immutable input:
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`.
The selected annotation input remains iteration 67's recorded enabled-Print
subset. Production import emits RTL with empty stderr and preserves all 34
existing definitions, all 1,713 retained annotation records and the Print
decoder header. The native source is the current
`circt-ingestion/post-fame-f1-shim.mlir` boundary. Both imported candidates retain
all 28 hardware definitions and the three exact LoadMem register descriptors.

Exact read-only RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
Comparison matches SimulationMaster's five register widths, four reset values,
ordered INIT_DONE/PRESENCE_READ/PRESENCE_WRITE slots, and the LoadMem contract:
sixteen memory pins, 34-bit addresses, 64-bit memory data, 75 widget equations,
sixteen identity routes, depth-two write requests and queued-write priority
over zero fill. These checks cover shared SFC behavior; the oracle disables
Print. No immutable fixture is modified and no manager verification gate is
started in this iteration.
`PrintRocketMMIOCompare.py` also matches all eleven native SFC AW/AR
regions/slaves. The two focused linked catalogs emit the same thirteen-bank
allocation: Print at 544/576, three CPU count words at 624 and ResetPulse at
640, with the complete LoadMem region retained. Its report is under
`iteration125-control-catalog/mmio-comparison/`.

This is production definition ingestion, not full live platform attachment.
Next smallest step: import the native TracerV/bridge token and stream boundary,
invoke checked clock-analysis transfer while retaining the existing Print
clock map, then compose the expanded CPU/control path and complete driver
collateral transaction. Enabled-Print production runtime, the SFC UART-bearing
differential and the overall compiler migration remain incomplete.

### Live TracerV binding beside production Print hosts (iteration 126)

The enabled-Print host boundary already retains all twelve Rocket TracerV
tokens and their analyzed rational clock. `bindPrintBridgeTracerV` validates
that existing channel clock map, then attaches the shared CIRCT TracerV engine
directly to `GGPrintBridgeHostWrapper`. No second target hierarchy is imported.
The generalized token attachment preserves the original Print host module and
every other port, binds the ten incoming payloads and two outgoing trigger
tokens, and publishes the live `GGTracerVTokenEngine>hPort` annotation target.
Each per-token ready/valid predicate excludes that token's own handshake,
matching `HostPortIOConnectChannels2Port` in Scala `midas/core/FPGATop.scala`.
The host hardware continues to implement the existing TracerV consumption,
512-bit cycle/PC packing and trigger history through FIRRTL operations.

The production compiler exposes this boundary with
`--compile-baseline --stop-after-print-host-binding --map-print-host-tracerv`.
It accepts the native control catalog option from iteration 125. TracerV
control and stream ports remain explicit; combining this stage with CPU stream
allocation rejects early until trigger configuration and queue attachment are
ported to the expanded boundary.

Evidence is under the mutable generated U250 directory
`iteration126-print-tracerv/`: production MLIR, RTL and retained annotations,
focused binding IR, build/CTest logs, CLI rejection report and semantic
`comparison.json`. Three focused CTests pass. The production-bound Print replay
checks twenty atomic rejection cases (malformed rational clocks, missing or
duplicate clock maps/channels, duplicate constructor and wrong latency), live
clock-target resolution, unchanged Print hardware and repeat rejection.
Three invalid CLI combinations reject before loading inputs.

The read-only SFC input and annotation oracles are
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`
and its sibling `firechip.chip.FireSim.FireSimRocketConfig.anno.json`.
The selected enabled-Print annotation subset remains iteration 67's input.
The candidate preserves all 61 preexisting module bodies, all 1,713 annotation
records apart from target retargeting and added TracerV channel endpoints, and
the sole analyzed clock map. All twelve local names, latency-one directions
and constructor widths match the immutable annotation oracle. The retained
TracerV clock is the named 1/1 domain
`uart_clock,clock_1000.0MHz,harnessbinder_clock,reference`.

Exact RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintTracerVBindingCompare.py` verifies all twelve actual wrapper payload
connections and aggregate/excluded handshake predicates. It compares emitted
TracerV host-port widths, 512-bit stream width, five handshake/trigger outputs
and stream payload against SFC `TracerVBridgeModule`. All 1,024 Boolean input
combinations with seeded 40-bit PCs and 64-bit cycles match: 6,144 output
samples. The engine body also matches the existing native F1Shim boundary
exactly. Existing TracerV sequential behavior and header CTests pass. These
checks cover shared SFC behavior; the immutable oracle disables Print. No
fixture was edited and no manager verification gate was started.

Next smallest step: attach the existing fifteen-word TracerV trigger bank and
6144-entry stream queue to this expanded live boundary, retain the Print host
registry for CPU allocation, and compose their control/stream attachments.
Enabled-Print production runtime, the SFC UART-bearing differential and the
overall compiler migration remain incomplete.

### Live Print/TracerV trigger configuration (iteration 127)

The production enabled-Print path now accepts
`--compile-baseline --stop-after-print-host-binding --map-print-host-tracerv
--map-print-host-tracerv-trigger`. This attaches the existing native
fifteen-word TracerV configuration bank to the live token engine, producing
`GGTracerVTriggerWrapper`. Host clock/reset drive the bank, masked trace
valid/PC/instruction drive its host-cycle trigger state, and the engine's
64-bit target cycle drives the delayed cycle-range trigger. The wrapper
forwards Print control, stream and count ports and exposes decoded
`tracerv_mcr`; stream queue and platform MMIO composition remain pending.

`addTracerVTriggerConfig` privately constructs the canonical bank before
changing the target boundary. A bank imported by the Rocket control catalog
is reused only after complete CIRCT operation/port/metadata equivalence.
Declaration locations and the textual parser's empty `parameters` default
are normalized for comparison. Hardware, constructor and register-map
mismatches reject atomically; the original imported module handle survives.
No Scala source, immutable fixture, or verification harness was changed.

Evidence is in the mutable generated U250 directory
`iteration127-print-tracerv-trigger/`: production MLIR/FIRRTL/annotations/RTL,
fresh-bank replay, final build and CTest logs, four early CLI rejections, and
`comparison.json`. Two focused CTests pass. The native TracerV test covers
26,144 packet/next-state samples, 4,096 helper samples and 22,304 trigger-state
samples; the added tests check in-memory and reparsed bank reuse, differing
source locations, six atomic hardware/metadata collisions and no duplicate
bank. Both creating a fresh bank and reusing the real catalog bank succeed.
The two bank bodies match the existing native F1Shim definition.

The exact immutable RTL comparison is
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintTracerVTriggerCompare.py` parses the emitted candidate and SFC
`TracerVBridgeModule` clocked register assignments and unsigned expressions.
It compares all 22 trigger register widths and next states, the three engine
control outputs and two read words for 4,096 sampled pre-edge states:
110,592 comparisons pass. Cases include host reset, target-reset trace
masking, coincident start/end boundaries, zero instruction masks, selectors
0 through 5, delayed PC/cycle limits and simultaneous decoded writes, with
HostPort transfer stalled. These are sampled semantic checks, not a proof
of equivalence. Write-only assertion behavior is covered separately by the
native bank test, rather than the emitted-RTL evaluator.

The candidate preserves 62 preexisting module bodies (including Print hosts
and the catalog bank), apart from adding the trace observation port to the
existing token wrapper. All 1,713 annotation records retain payload/class/order
apart from target retargeting, and the analyzed clock map is unchanged. The
emitted wrapper's actual bank/engine clock, trace and bidirectional control
connections match. The SFC FIRRTL input remains
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`,
with iteration 67's selected enabled-Print annotations. The immutable SFC
platform disables Print; no enabled-Print full-runtime equivalence is claimed.
No manager verification gate was started.

Next smallest step: attach the existing 6144-entry TracerV stream queue to
`GGTracerVTriggerWrapper`, preserve the inner Print host registry, then compose
the expanded CPU-stream/count and platform MMIO/control path. Enabled-Print
production runtime, the SFC UART-bearing baseline and the overall port remain
incomplete.

### Live Print/TracerV stream queue and build repair (iteration 129)

The interrupted iteration 128 left the expanded queue attachment in the live
worktree. Its manager `replacertl` failed while compiling the new nested
annotation fixture in `TracerVStreamQueueTest.cpp`: mismatched parentheses
caused cascading initializer-list errors. Full stdout, stderr and the manager
log `2026-10-12--07-03-36-replacertl-UTI5HXCI0BG5P69M.log` were inspected.
The fixture now constructs targets, nested dictionary and annotation in named
steps. Production `sim/make/goldengate.mk` explicitly builds the
`goldengate-circt` target; focused validation separately builds its test
executables. The native compiler and repaired queue test both build successfully.
No manager gate was rerun by the implementation agent.

The production enabled-Print compiler replay now accepts the existing token
and trigger flags followed by `--map-print-host-tracerv-queue`. It uses
`attachTracerVStreamQueue` to wrap `GGTracerVTriggerWrapper` in
`GGPrintTracerVStreamQueueWrapper`, buffering only the TracerV stream. The
original standalone queue entry point still selects its original boundary.
All other ports, inner Print bodies and the Print host registry are retained.
The new boundary exposes the thirteen-bit TracerV occupancy count; producer
stream targets stay on the inner module, copied port targets move to the
wrapper, and nested raw annotation targets are updated recursively. Seven
expanded invalid-boundary cases reject without changing IR.

Evidence is in the mutable generated U250 directory
`iteration129-print-tracerv-queue/`: build and CTest logs, production
MLIR/FIRRTL/annotations/RTL, five early CLI rejections, queue comparison and
the shared expression-parser trigger regression. The queue CTest passes with
28,303 pushes, 28,213 pops and 343 read/write collisions through actual native
memory operations, including full/empty stalls and reset-time writes.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
module `Queue_50`, instantiated as the TracerV outgoing queue in SFC's
CPUManagedStreamEngine. `PrintTracerVQueueCompare.py` evaluates emitted scalar
register assignments and expressions: 4,496 boundary/random states yield
44,960 matching pointer/read-address next-state, ready/valid/count and memory
write comparisons. Both sides capture the lookahead address without reset
and allow RAM writes during reset. The 6144x512 RAM geometry, memory interfaces
and actual wrapper connections match structurally; this evaluator does not
simulate RAM arrays. The native CTest independently covers payload ordering.
The unsigned sized-cast expression support added to the shared test evaluator
also passes the previous 110,592 SFC trigger comparisons.

All 64 preexisting native module bodies, including the trigger bank and Print
host registry, remain identical. The new queue body matches the existing
native F1Shim queue; all 1,713 annotation payloads/classes/order and the
analyzed clock map are preserved. The SFC FIRRTL input and selected enabled
Print annotations are the same as iteration 127. Neither immutable fixture
was edited. This comparison covers shared queue semantics; the immutable
platform has Print disabled and enabled-Print full runtime remains unverified.

Next smallest step: discover the preserved inner Print host registry through
the TracerV wrappers, allocate both CPU streams and their occupancy registers,
then compose TracerV and Print MMIO/control paths. The manager gate must still
verify the build repair; the SFC UART-bearing baseline and overall port remain
incomplete.

## Iteration 130: live Print/TracerV CPU stream and occupancy allocation

The iteration-129 harness confirms CIRCT replacertl, bare Verilator smoke,
90,141 portable checks (`0x78194504c338c229`) and 90,805 Rocket checks
(`0x5f3744639d41ea35`) passed. Rocket differential comparison still awaits
an SFC UART-bearing baseline. Those gates exercise the baseline platform;
they do not validate the staged enabled-Print boundary introduced here.

`mapPrintBridgeCPUStreams` now resolves the bound Print registry through the
active top's retained `sim` instance chain. It checks unique registry metadata,
acyclic reachability, matching port types/directions and unique direct SSA
forwarding of each stream, occupancy count, control port and host clock/reset
before staging any mutation. Existing bound host instance, local control bank,
queue geometry and ordered identity checks remain in effect. The original
Print module and registry stay intact; allocation commits the read/count
wrappers and recursive annotation transfers together.

The production compiler accepts the token, trigger and queue flags followed by
`--map-print-host-cpu-streams`. It derives the preceding TracerV allocation
from the actual live queue instance and appends Print, preserving Scala's
stream order. The replay produces TracerV/Print source indices 0/1, 512-KiB
windows at 0/524288, and read-only occupancy words at offsets 0/4. Joint Rocket
control dispatch is explicitly rejected until the widget assembly is ported.

Evidence is under the mutable generated U250 directory
`iteration130-print-tracerv-cpu/`: native compiler build, nested allocation
tests, multistream semantic tests, production MLIR/RTL/annotation outputs,
early CLI rejection logs and `comparison.json`. The nested test preserves
three forwarding wrappers and rejects nine malformed forwarding/identity
cases atomically, alongside the existing eight allocation rejections.
The native multistream interpreter passes 196,241 SSA cases, 41,088 R
transfers, 36,992 unselected-counter updates and 27 atomic rejections.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
module `CPUManagedStreamEngine`. `PrintTracerVCPUCompare.py` evaluates emitted
AR/R handshake, selected 512-bit data, ID, last, queue readiness and live
count expressions for both windows against the SFC stream-0 contract.
4,096 directed/random cases yield 49,152 matching signal checks. SFC has
optimized away the response-code port; its constant is recovered from the
actual `FPGATop.axi4buf_2` / `AXI4Buffer_2.nodeIn_r_deq_q` / `Queue_55` RAM
writer. Count zero extension matches the SFC engine's thirteen-bit queue
count. Native and emitted-RTL connections attach each payload and count to
the corresponding retained live instance.

All 66 preceding module bodies, 1,713 annotation payloads/classes/order and
the analyzed clock map are unchanged. Input is the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, with the selected enabled
Print annotations from iteration 67. Neither immutable fixture is edited.
The recorded SFC platform has Print disabled: these shared transport checks
and native semantic tests do not establish full enabled-Print runtime parity.
No manager verification step was started by the implementation agent.

Next smallest step: compose the live TracerV fifteen-word bank and Print local
control banks with the shared CPU count bank in the Rocket control decoder,
then connect joint read/write dispatch before platform/header assembly.

## Iteration 131: live TracerV/Print/count MMIO request assembly

The iteration-130 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks. The SFC UART-bearing baseline
remains pending. These baseline gates do not validate enabled-Print runtime.

The compiler now accepts the live TracerV token/trigger/queue and CPU stream
flags together with `--map-print-host-control`. Before queue construction,
`mapPrintBridgeTracerVControl` binds the actual fifteen-word trigger bank to
the native 25-bit-address/12-bit-ID AXI MCR adapter. An imported adapter is
reused only after exact operation equivalence, ignoring declaration locations
and the parser's empty parameter default. Comparison normalizes a separate
copy; existing hardware and its argument locations stay unchanged. Attachment
stages and verifies all new hardware and annotation transfers atomically.

Request dispatch resolves the live adapter through the unique retained `sim`
path, verifies direct AXI/clock/reset forwarding and actual decoded-bank
connections, and requires a corresponding allocated TracerV CPU stream.
Scala `Widget.scala` size sorting places TracerV's 15 words at byte 0 (64-byte
region), Print's six words at 64 (32 bytes), and the two read-only occupancy
words at 96 (8 bytes). Native AW/W/AR dispatch consumes all three banks;
B/R response assembly remains explicit, and the joint response CLI flag rejects.

Evidence is in the mutable generated U250 directory
`iteration131-print-tracerv-control/`: successful SFC-handoff compiler replay,
MLIR/RTL/annotations, seven passing request/allocation CTests, focused live
composition/catalog-reuse tests with ten atomic rejection cases, and both
comparison reports. The initial signature-location failure is retained as
`attempt1.stdout.log`/`attempt1.stderr.log` alongside the corrected replay.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintControlDispatchCompare.py --tracerv` matches eight `NastiRouter` request
predicates, all three banks' actual request/readiness nets, and preserved
module bodies and 1,713 ordered annotations including exact target transfers.
`PrintTracerVCPUCompare.py` again matches 49,152 `CPUManagedStreamEngine`
handshake/data/count signals; all 67 preceding bodies and analyzed clocks stay
unchanged. A fresh Print-only compiler replay and dispatch comparison also
pass under `compatibility/`.

Input remains the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, with iteration-67 selected
Print annotations and the native Rocket control catalog. Neither immutable
fixture was edited. SFC has Print disabled: shared transport comparisons do
not establish enabled-Print platform/runtime parity. No manager verification
step was started. Next smallest step: assemble the joint B/R arbiters and ID
trackers for these three banks, then attach the platform control master.

## Iteration 132: live TracerV/Print/count B/R response assembly

The iteration-131 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks; its SFC UART-bearing baseline
remains pending. These baseline gates do not establish enabled-Print runtime.

`--map-print-host-responses` now composes the selected live TracerV path with
Print and CPU occupancy banks. The composer re-derives the fifteen TracerV
trigger words, six Print configuration words and live CPU count allocation,
then checks every decoder region and matching AW/W/AR bank identity against
that allocation. It checks the actual AR slave connections even for selected
composition, so equally stale read/write catalogs cannot redirect responses.
The native read tracker, burst-locking R arbiter, B arbiter and write tracker
consume the three banks plus the error endpoint. All four passes are staged
and verified before committing new FIRRTL modules and annotation transfers.
The joint master flag still rejects: its allocated Print header requires the
next change to support the additional TracerV region.

Evidence is in the mutable generated U250 directory
`iteration132-print-tracerv-responses/`: the successful compiler replay,
`production/post-print-control-responses.{mlir,sv}`, all annotations, focused
native composition with twelve atomic rejection cases, and comparison reports.
The focused cases cover missing/aliased banks, stale slave identities and
region bounds, removed/swapped/duplicate AR drivers, missing register metadata,
missing annotations, a late write-tracker collision, and a detached Print host.
A fresh Print-only replay under `compatibility/` also passes the same response
comparison. The broader native binding test passes after renaming its generic
preceding stream fixture: it had used the reserved live `tracerv_stream` name
without a TracerV control bank. The initial test failure is retained in
`iteration132-ctest.log`; the corrected run is in `print-binding-final.log`.
The actual Rocket queue/control fixture retains its reserved TracerV names.
Its thirteen-bank catalog allocation passes both constructor orders and forty
atomic rejections, recorded in `rocket-catalog.stdout.log`.
The write tracker and both arbiters pass their full native CTest matrices.
The broad read-tracker run was interrupted after 466.50 seconds, with its
log preserved; it is not claimed as passing. `ControlReadTrackerTest.cpp`
now accepts an optional `--regions N` while retaining the full default matrix
and all malformed-boundary tests. The selected two- and three-bank runs retain
1,500 randomized cycles, directed ID collisions/full-queue/reset/final-beat
checks, empty bindings and atomic rejections; evidence is in
`read-tracker-{2,3}.{stdout,stderr}.log`.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintControlResponsesCompare.py --tracerv` matches eight `NastiRouter`
retirement predicates and response IDs, request acceptance/capacity/ID/route
connections, both 64-slot reorder queues and 12-bit tags, and each bank's actual
R/B payload/readiness/valid wiring. Normal SFC MCR R.last is optimized to one;
the native general boundary retains it, as does the SFC error endpoint. The
comparison checks all 84 retained module bodies, 1,713 ordered annotation
payloads and exact recursive target transfers through four wrappers, and the
unchanged analyzed clock map. The request comparison still matches its eight
SFC handshake predicates and all three banks' existing request connections.

Input is the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, with the selected Print
annotations from iteration 67 and the native Rocket control catalog. Neither
immutable fixture was edited. The recorded SFC has Print disabled; these shared
transport comparisons do not establish enabled-Print platform/runtime parity.
No manager verification step was started. Next smallest step: extend allocated
Print header validation to the joint live register map, then attach the native
platform control master and compare its addresses and stream indices.

## Iteration 133: joint TracerV/Print control master and allocated collateral

The iteration-132 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks. Its SFC UART-bearing baseline
remains pending; these baseline gates do not establish enabled-Print runtime.

`--map-print-host-master` now accepts the live selected TracerV/Print/count
pipeline and attaches the native `GGControlMasterWrapper` through the existing
CIRCT FIRRTL control-master pass. Allocated-header preparation re-derives
TracerV's fifteen-word bank and validates its live stream identity and matching
read/write binding along with Print and CPU count banks. It uses the Scala
`Widget.scala` registration order and stable descending region-size allocation;
Print constructor indices remain independent of CPU stream order. The resulting
map has TracerV at byte 0 (64 bytes), Print at 64 (32 bytes), and CPU counts at
96 (8 bytes). The Print constructor uses widget index 0 and stream index 1,
six MMIO addresses 64 through 84, DMA base 524,288 and count address 100.
Header generation preserves IR and existing platform collateral.

Evidence is in the mutable generated U250 directory
`iteration133-print-tracerv-master/`. Native compiler and binding-test builds
pass. `goldengate-print-binding-test --print-tracerv-allocated-header` consumes
the iteration-132 response MLIR, verifies the newly bound control master and
read-only header generation, and passes eight atomic rejections: missing,
truncated or duplicate-offset Trace registers; changed Trace stream name or
port; removed control binding; aliased port; and stale slave identity. The
default `goldengate-print-binding` CTest passes as well. A fresh compiler replay
produces `candidate/post-print-control-master.{mlir,sv}` and
`candidate/print-bridge-allocated.const.h`. That header compiles as C++17 against
the actual FireSim driver headers under all four emitted GET_* guards.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintControlMasterCompare.py` matches all 45 SFC `FPGATop` control-port
widths/directions and all corresponding native master-to-router connections;
all 92 preceding FIRRTL module bodies, copied ports and recursively retargeted
annotation payloads stay unchanged. `PrintAllocatedHeaderCompare.py --tracerv`
matches the SFC fifteen-word Trace bank and 64-byte region, and CPU queue depth
6,144, 512-bit beat, read-only 32-bit count word and 524,288-byte DMA window.
Absolute addresses differ explicitly: selected Trace 0 versus SFC 256, and
Print count 100 versus SFC Trace count 568, due to their different widget
catalogs. Fresh Print-only compatibility replay passes both comparisons
(86 retained modules, stream 0, count address 32).

Input is the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, iteration-67 selected Print
annotations and the native Rocket control catalog. Neither immutable fixture
was edited. The SFC fixture disables Print; shared ABI comparisons do not
establish enabled-Print platform/runtime parity. The emitted fragment constructs
Print and the CPU stream engine; it still lacks an allocated TracerV driver
constructor and full platform attachment. No manager verification step was
started. Next smallest step: derive TracerV constructor collateral from this
same joint live allocation, before attaching the complete platform wrapper.

## Iteration 134: allocated TracerV constructor in joint Print collateral

The iteration-133 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks. The SFC UART-bearing runtime
baseline remains pending; these gates cover the baseline compiler path.

`preparePrintBridgeAllocatedHeader` now composes the existing native TracerV
header emitter with Print and CPU stream collateral for a selected joint
control master. The emitter reads CIRCT FIRRTL operations and validates live
instance paths, actual trigger-register reset/update/read SSA, constructor
payload, analyzed rational clock, queue storage and ordered stream selectors.
It runs on the existing scratch clone and publishes caller text only after
the whole batch succeeds. Existing Rocket driver assembly continues to own its
TracerV constructor, avoiding duplicate registration.

Evidence is in the mutable generated U250 directory
`iteration134-print-tracerv-collateral/`. Native compiler, binding and TracerV
header targets build successfully. The focused binding test passes eleven
atomic rejections, including changed traceEnable reset state, missing token
engine constructor and missing Trace clockInfo. The default Print binding and
TracerV header CTests pass. A fresh compiler replay emits
`candidate/print-bridge-allocated.const.h` with one `tracerv_t` constructor,
widget/stream index 0, fifteen addresses 0 through 56, queue depth 6,144,
retirement width 1 and clock ratio 1:1. Print retains widget index 0, stream
index 1, six addresses 64 through 84 and count address 100. All four emitted
GET_* guards compile as C++20 against the actual FireSim driver headers,
including the firechip TracerV header.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintAllocatedHeaderCompare.py --tracerv --tracerv-annotations ...` recovers
all fifteen register names and word offsets from SFC executable assignments,
including reset/write muxes and truncated words, and matches the emitted
constructor. SFC queue depth, beat size, count permissions and DMA geometry
also match. Selected TracerV base 0 versus SFC 256, and Print count 100 versus
SFC Trace count 568, remain explicit catalog differences. The control-master
comparison matches all 45 SFC control ports and preserves 92 preceding module
bodies, copied ports and annotation identities.

Exact immutable annotation and clock artifacts in the primary fixture:
`sims/firesim-staging/generated-src/firechip.chip.FireSim.FireSimRocketConfig.sfc-golden-2026-10-01/firechip.chip.FireSim.FireSimRocketConfig.anno.json`
and `firechip.chip.FireSim.FireSimRocketConfig.firesimRationalClockBridge.freq-summary`
in that same directory. TraceBundleWidths (1/40/32/64/40) and the three
recorded 1:1 clock aliases match the constructor payload and clock record.
The fixtures contain no generated C++ header; this is a structured comparison
against their RTL, constructor annotation and frequency summary.

Neither immutable fixture was edited. The SFC fixture disables Print, so this
does not establish enabled-Print platform/runtime parity. No manager gate was
started. Next smallest step: attach the selected joint control/stream boundary
to the native platform wrapper and compare its external ports and allocation.

## Iteration 135: selected Print CPU empty writer and AW/W buffering

The iteration-134 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks. Its SFC UART-bearing runtime
baseline remains pending. No manager verification step was started here.

The selected joint control master exposes only CPU AR/R; it cannot yet attach
the complete platform slave. `--map-print-host-cpu-write-buffer` now requires
`--map-print-host-master` and adds the Scala zero-sink AW/W/B defaults and two
independent depth-two AW/W queues. `materializeEmptyCPUStreamWrite` shares the
existing baseline operation emitter with this selected boundary. The Print
composition validates live allocated driver/stream/control identities before
staging both wrappers, the empty writer and queue memories/registers on a
clone. Verification precedes publication of additions and recursive annotation
retargeting. Existing modules, operations, SSA values and allocation metadata
remain intact. Incoming CPU streams are still unsupported.

Evidence is in the mutable generated U250 directory
`iteration135-print-cpu-write-buffer/`. Native compiler, Print binding, empty
writer and AW/W buffer targets build successfully. Fresh joint TracerV/Print
and Print-only compiler replays emit
`{candidate,compatibility}/post-print-cpu-write-buffer.{mlir,sv}` and corresponding
annotation archives. The joint allocated header is unchanged from iteration
134. The compiler rejects the new option without its required master stage.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintSelectedCPUWriteCompare.py` matches `CPUManagedStreamEngine` live AW-ID
forwarding, zero ready/B-valid/response, reset-gated size-six/full-strobe
assertions and `Queue_51`, `Queue_52`, `AXI4Buffer_2` payload/storage/reset and
handshake contracts. SFC eliminates the W dequeue and specializes its read
address to zero; the native queue retains the general dequeue, whose ready is
bound to the empty writer's zero. AW and W each retain two payload entries,
no flow/pipe, asynchronous reads and synchronous writes, including accepted
RAM writes during reset while occupancy resets empty.

The generated empty writer passes 73,984 interpreted operation cases, all
65,536 live AW IDs and every write strobe lane. Each generated AW/W queue passes
20,400 cycles against a separate deque oracle, including full/empty stalls,
simultaneous push/pop and reset-time writes. The structured comparison preserves
93 prior module bodies, 89 final public ports and 100 buffered routes for the
joint candidate; Print-only preserves 87 bodies, 97 ports and 108 routes.
Both retain all archived annotation payloads, recursive target transfers,
clock analysis and MMIO/CPU stream allocation metadata. The five additions are
ordinary FIRRTL operations, not imported Scala or an SFC compiler fallback.
The selected binding test passes 11 atomic rejections, preserved operation and
module identities, and recursive copied targets. It discovers ordered hosts
from the binding registry, since helper modules also retain Print metadata.
The Print binding, empty CPU writer and CPU write buffer CTests pass (3/3).

Input remains the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, iteration-67 selected Print
annotations and the native Rocket control catalog. Neither immutable fixture
was edited. The fixture disables Print, so shared transport matches do not
establish enabled-Print platform/runtime parity. The boundary still exposes
unimplemented bridge inputs and lacks the host-memory port and CPU AR/R/B
buffers. Next smallest step: attach the independent native AR queue while
preserving the joint allocation, then add R/B buffering before platform ports.

## Iteration 136: selected Print independent CPU AR buffering

The iteration-135 harness passed CIRCT replacertl, Verilator bare smoke,
90,141 portable checks and 90,805 Rocket checks. Its SFC UART-bearing baseline
remains pending. No manager verification step was started here.

`--map-print-host-cpu-read-buffer` requires the selected AW/W buffer stage and
attaches `GGSelectedPrintCPUStreamReadBufferWrapper`. It reuses the native AW
payload queue definition in a separate AR instance, with independent memory,
pointers and occupancy. The wrapper forwards six AR signals through this queue
and copies every other interface. CPUManagedStreamEngine retains the AR head
until grant, R ready, stream valid and lastReadBeat all hold. The composition
validates live Print allocation, stages on a clone, verifies, then publishes
only the wrapper and recursively retargeted annotation archive. Existing
modules, operations, SSA values, ports and allocation metadata stay intact.

The first native replays exposed an allocation-validator mismatch: selected
headers forbade the empty writer added by iteration 135. Buffered selected
boundaries now require one live incoming engine with zero sink streams and
validate its executable zero AW-ready, W-ready and B-valid defaults. Earlier
outgoing-only boundaries retain their stricter omission checks. Both buffered
boundary names are accepted by the header emitters, whose live hierarchy,
stream/count SSA and MMIO allocation checks remain in force. Incoming streams
remain unsupported. Focused rejection cases cover a nonzero sink count and a
changed AW-ready default in addition to missing allocation, queue and port data.

Evidence is in the mutable generated U250 directory
`iteration136-print-cpu-read-buffer/`. Native compiler, Print binding, CPU AR
buffer, managed stream header and CPU read targets build successfully. A final
header guard edit occurred during the broad rebuild; the final native compiler
object and link were rerun using CMake's exact generated commands before the
successful replays (`final-native-link.log`). Joint TracerV/Print and Print-only
replays emit `{candidate,compatibility}/post-print-cpu-read-buffer.{mlir,sv}`
and annotation archives. The new CLI option rejects omission of its AW/W stage.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintSelectedCPUReadCompare.py` matches `Queue_51`, `AXI4Buffer_2` and
`CPUManagedStreamEngine`: depth two, no flow or pipe, ID/address/length/size
widths 16/64/8/3, reset flushing occupancy while accepted enqueue still writes
RAM, all fourteen AR queue routes, final-beat dequeue and retained AR response
ID. The shared native queue is identical to the baseline operation emitter.
The joint candidate retains 98 preceding module bodies, 89 public ports,
83 copied ports and 97 wrapper connections; Print-only retains 92 bodies,
97 ports, 91 copied ports and 105 connections. Both preserve the complete
annotation archive and clock/control/stream analysis metadata through recursive
copied/consumed target transfer.

Each actual generated AR queue passes 21,450 interpreted cycles against an
independent deque oracle, including full/empty pressure, simultaneous enqueue
and dequeue, reset-time writes and held burst heads. The focused binding test
passes thirteen atomic rejections on each variant, preserves existing operation
and module identities, checks nested copied and consumed targets, and requires
allocated driver headers before and after AR wrapping to be identical. The
Print binding, CPU managed stream header, CPU stream read and CPU AR buffer
CTests pass (4/4).

Replay input remains the primary immutable fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, the iteration-67 selected
Print annotations and the native Rocket control catalog. Neither immutable
fixture was edited. The SFC fixture disables Print, so these shared transport
matches do not establish enabled-Print platform/runtime parity. R/B buffering
and host-memory/platform attachment remain pending. Next smallest step: attach
and compare the selected boundary's native depth-two CPU R response queue.

## Iteration 137: selected Print CPU R response buffering

The iteration-136 harness passed CIRCT replacertl and Verilator bare, portable
(90,141 checks) and Rocket (90,805 checks) workloads. The SFC UART-bearing
runtime baseline remains pending. No FireSim manager verification step was
started in this iteration.

`--map-print-host-cpu-response-buffer` requires the selected AR buffer stage and
attaches `GGSelectedPrintCPUStreamResponseBufferWrapper` plus the shared native
`GGCPUStreamRQueue2`. CPUManagedStreamEngine R fire now enqueues a response;
host R dequeue independently drains it. ID/data/response/last are retained
under host backpressure. The read engine retires the AR head on the final
response enqueue, as in FPGATop's default AXI4Buffer. The selected composition
validates live Print allocation, stages and verifies on a clone, and publishes
only the new queue/wrapper and recursively retargeted archive. Existing module,
operation, SSA, port and allocation identities remain intact. Both allocated
header emitters accept the new selected boundary while retaining their existing
live hierarchy and zero-sink writer checks.

The first focused rejection run found that R attachment accepted a malformed
preceding AW/AR helper (queueDepth=3). The existing native AR interface/storage
validator is now shared as `validateCPUStreamAddressQueue` and required before
selected R attachment. This also rejects changed memory read latency without
mutating live IR. The successful final runs cover thirteen atomic rejections,
recursive consumed R and copied control/clock/AR targets, unchanged module and
operation identities, and identical allocated driver headers before/after
wrapping. The refactored selected AR tests pass on both variants as well.

Evidence: the mutable generated U250 directory
`iteration137-print-cpu-response-buffer/`. Native compiler, Print binding,
CPU managed stream header, AR buffer and R buffer targets build. Final joint
TracerV/Print and Print-only replays emit
`{candidate,compatibility}/post-print-cpu-response-buffer.{mlir,sv}` and
`post-print-cpu-response-buffer-all.json`. The option rejects a missing AR
stage. Print binding, CPU managed stream header, AR buffer and R buffer CTests
pass (4/4). Each actual candidate R queue passes 20,400 interpreted cycles
against an independent deque oracle: 7,904 pushes, 6,881 pops, 346 reset-time
writes, 1,089 full-with-pop blocks and 1,503 empty-with-push blocks. Coverage
includes held complete payloads, simultaneous push/pop, occupancy reset and
accepted reset-time RAM writes.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintSelectedCPUResponseCompare.py` matches Queue_55/AXI4Buffer_2's R contract:
depth two, no flow or pipe, payload widths 16/512/2/1, stored Okay response,
control-only reset flushing and accepted reset-time RAM writes. It also checks
Queue_51 and CPUManagedStreamEngine's final-beat AR dequeue and retained R ID.
The native R queue is identical to the baseline operation emitter. All fourteen
native R routes match; SFC specializes the constant response into its RAM write
and exposes thirteen routes. The joint candidate preserves 99 preceding module
bodies and 89 ports (83 copied, 97 wrapper connections); Print-only preserves
93 bodies and 97 ports (91 copied, 105 connections). Both preserve the complete
annotation archive and clock/control/stream analysis metadata. The final report
is `sfc-comparison-final.json`.

Replay input is the primary immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, the iteration-67 selected
Print annotations and the native Rocket control catalog. Neither fixture was
modified. Large intermediate FIRRTL/MLIR/RTL replay outputs are compressed;
the AR/R handoffs and the AW/W handoff used by the focused tests remain raw.
The recorded SFC fixture disables Print, so shared transport parity does not
establish enabled-Print runtime parity. B buffering, host-memory/platform
attachment and enabled-Print runtime remain pending. Next smallest step:
attach and compare the selected native depth-two CPU B response queue.

## Iteration 138: selected Print CPU B response buffering

The iteration-137 harness passed CIRCT replacertl and Verilator bare, portable
(90,141 checks) and Rocket (90,805 checks) workloads. The SFC UART-bearing
runtime baseline remains pending. No FireSim manager verification step was
started in this iteration.

`--map-print-host-cpu-write-response-buffer` requires the selected R buffer
stage and attaches `GGSelectedPrintCPUStreamWriteResponseBufferWrapper` plus
`GGCPUStreamBQueue2` through FIRRTL operations. FPGATop's default AXI4Buffer
and CPUManagedStreamEngine's zero-sink branch are the oracle: writer B valid
stays zero, response is Okay, and ID comes from AW. Preserve the outer queue's
RAM reads when invalid rather than bypassing storage with the live writer ID
or response. The selected composition validates the preceding AW/AR geometry
and live Print allocation, stages and verifies on a clone, then publishes only
the new queue/wrapper and recursively transferred archive. Existing module,
operation, SSA, port and allocated header identities are retained. The two
allocated header emitters accept this new boundary and keep all live hierarchy,
stream count and empty-writer checks. Hierarchy analyses must be rebuilt after
wrapping; no annotations are added or consumed.

Evidence: the mutable generated U250 directory
`iteration138-print-cpu-write-response-buffer/`. Native compiler, Print binding,
CPU managed stream header and B buffer targets build. Joint TracerV/Print and
Print-only native replays emit
`{candidate,compatibility}/post-print-cpu-write-response-buffer.{mlir,sv}` and
`post-print-cpu-write-response-buffer-all.json`. The CLI rejects a missing R
stage. Print binding, CPU managed stream header and B buffer CTests pass (3/3).
Both selected B composition tests use the preserved iteration-137 R handoffs
and pass thirteen atomic rejections, nested copied/consumed target transfers,
unchanged module/operation identities and identical allocated headers. The
shared focused AR and R branches also pass on the new joint replay handoffs.
Each actual emitted B queue passes 20,400 interpreted cycles against an
independent deque oracle: 7,878 pushes, 6,933 pops, 295 reset-time RAM writes,
1,045 full-with-pop blocks and 1,499 empty-with-push blocks. A further 1,024
zero-sink cycles preserve RAM contents and invalid payload reads through reset.

Exact immutable RTL oracle:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintSelectedCPUWriteResponseCompare.py` matches Queue_53, AXI4Buffer_2 and
CPUManagedStreamEngine's B contracts: depth two, ID16/response2, control-only
reset, zero valid/Okay writer and RAM reads when invalid. The generic native
queue is identical to the baseline operation emitter and independently tested
with traffic. All ten native B routes match; SFC specializes enqueue valid and
response, exposing seven B routes. Joint TracerV/Print retains 101 prior module
bodies and 89 ports (85 copied, 95 wrapper connections); Print-only retains 95
bodies and 97 ports (93 copied, 103 connections). Four consumed B port targets
stay on the retained inner module with the new circuit identity; all copied
ports transfer outward. The complete annotation archive and other analysis
metadata are retained. The report is `sfc-comparison.json`.

Replay input is the primary immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, the iteration-67 selected
Print annotations and native Rocket control catalog. Neither fixture was
modified. Large intermediate FIRRTL/MLIR/RTL outputs are compressed; R/B and
AW/W/AR handoffs used by comparisons and focused tests remain raw. The recorded
fixture disables Print, so transport parity does not establish enabled-Print
runtime parity. Host-memory/platform attachment and enabled-Print runtime remain
pending. Next smallest step: attach the selected buffered CPU scalars to the
native U250 aggregate AXI port adapter while preserving allocated collateral.


## Iteration 139: selected Print CPU aggregate U250 AXI port

Prior-iteration harness feedback reports CIRCT replacertl, Verilator bare,
portable FAME and Rocket suites passing. The Rocket UART-bearing SFC baseline
remains pending. No manager verification step or hardware command was started
in this iteration.

`--map-print-host-cpu-port` requires the selected B buffer boundary. The native
composition adds `GGSelectedPrintCPUStreamPlatformWrapper` and the shared
`GGCPUStreamPortAdapter` through FIRRTL operations. FPGATop.scala's
`cpuManagedAXI4NodeTuple` and `cpu_managed_axi4 <> node.out` are the oracle:
expose the U250 slave with address64/data512/ID16, forward 25 supported CPU
leaves, and leave 12 unused AW/AR metadata and W data/last inputs without users.
No state or protocol gating is added. The selected profile does not yet own a
platform memory port; the expanded profile retains its existing memory-port
requirement. Both still require the exact 25 scalar widths/directions, unique
child/B-queue forwarding, outgoing allocation and one zero-sink writer.

The selected composition validates preceding AW/AR queue geometry and live
Print/count/TracerV driver allocation, stages and verifies on a clone, then
publishes only the new wrapper and adapter. Every prior module, operation, SSA
value and copied port retains identity. Consumed CPU scalar targets remain on
the inner module; copied targets move outward, including nested annotation
arrays/dictionaries and control bundle components. Both allocated-header
emitters accept the new top and retain their live hierarchy, count/DMA and
empty-writer checks. Hierarchy analyses must be rebuilt after wrapping.

Mutable evidence: generated U250 `iteration139-print-cpu-port/`. The native
compiler, Print binding, CPU header and CPU port targets build. Joint Print/
TracerV and Print-only replays emit
`{candidate,compatibility}/post-print-cpu-port.{mlir,sv}` and
`post-print-cpu-port-all.json`. Print binding, CPU header and CPU port CTests
pass (3/3); the CLI rejects a missing B stage before reading input files. Both
focused selected composition tests use the preserved iteration-138 B handoffs
and pass 19 atomic rejection cases, identical allocated headers, recursive
target transfer and unchanged operation/module identities. The actual emitted
adapters pass identity-driver checks for every payload bit, including R512,
plus all 12 unused input fields. These checks inspect candidate FIRRTL
operations and do not substitute for enabled-Print runtime verification.

Exact immutable RTL artifact:
`sims/firesim/deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
`PrintSelectedCPUStreamPortCompare.py` matches FPGATop's 37-leaf ABI, all 25
buffered identity routes and 12 unused inputs. Each emitted candidate SV has
that exact CPU port ABI, and its adapter matches the native baseline module.
Joint Print/TracerV retains 103 prior module bodies, 64 copied ports (65 public
ports including the aggregate) and 90 wrapper connections. Print-only retains
97 bodies, 72 copied ports (73 public ports) and 98 connections. All preceding
module bodies and the complete recursively transferred annotation/analysis
archive match. Full results are in `sfc-comparison.json`.

Replay input is the primary immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`, selected iteration-67 Print
annotations and the native Rocket control catalog. Neither immutable fixture
was modified. Intermediate outputs are compressed after comparison; the B and
CPU-port MLIR and CPU-port SV handoffs used by tests remain raw. The fixture
disables Print: shared transport parity does not establish enabled-Print
runtime parity. Selected host-memory/platform integration remains pending.
Next smallest step: bind the selected target memory channels to the native
FASED host-memory boundary, preserving this CPU port and allocated collateral.

### Iteration 140: selected Print FASED target-token boundary

`mapPrintBridgeFASEDTokens` now composes the shared native FASED token engine
onto `GGSelectedPrintCPUStreamPlatformWrapper`, producing
`GGSelectedPrintFASEDTokenWrapper`. The CLI exposes the stage with
`--map-print-host-fased-tokens`, requiring `--map-print-host-cpu-port`.
It revalidates the live Print/optional TracerV allocation, stages and verifies
all additions, and publishes only the engine/wrapper and transferred raw
annotations. Existing module bodies and operation identities are retained.
The eleven memory channels become an ordinary FIRRTL HostPort with the SFC
own-predicate exclusion gates, targetFire, AXI ingress snoops and distinct
model/ingress/egress reset gates. The CPU AXI aggregate and other ports are
copied unchanged; Print/TracerV allocated driver text remains identical.

Evidence directory:
`sim/generated-src/xilinx_alveo_u250/xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config/iteration140-print-fased-tokens`.
Both native CLI replays (Print/TracerV and Print-only compatibility) passed,
including MLIR verification and simulator RTL/annotation emission. Native
build and three relevant CTest suites passed; after fixing test-only attribute
lookups, the binding suite passed again. Focused composition tests passed on
both replayed CPU-port boundaries, with 14 atomic malformed/repeat rejections,
unchanged allocated headers and retained module/SSA identities. The FASED
unit test also exercises the selected entry with 4096 token/reset/payload
cases, 8192 eleven-channel gate cases and 460 payload bindings. CLI dependency
preflight passed.

Exact immutable golden comparison:
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`,
module `FASEDMemoryTimingModel`. `PrintSelectedFASEDTokensCompare.py` passed
shared targetFire/ingress-offer, reset and AW/W/AR snoop equations, 256 Boolean
cases per candidate, eleven token gates and 46 payload routes. The native
engine matches `circt-ingestion/post-fame-fased-token.mlir`. Candidate and
compatibility preserve respectively 105/99 prior modules, 54/62 copied ports,
122/130 wrapper connections and all 1713 annotation rows with the intended
recursive target transfer. Report: `sfc-comparison.json`. The compiler replay
input is the immutable compiler fixture's
`firechip.chip.FireSim.FireSimRocketConfig.sfc.fir`; neither golden was written.

Scope remains partial: the golden disables Print, so this comparison proves
shared FASED semantics rather than enabled-Print runtime parity. Host idle,
ingress capacity, timing and response readiness remain exposed. Next smallest
step: attach the native host-outstanding counters to this selected boundary,
then the AW/W/AR ingress queues, preserving CPU and live allocated collateral.
Manager verification remains harness-owned; no manager gate was started here.
