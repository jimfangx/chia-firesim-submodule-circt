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

## Remaining scope

Shared SRAM definitions now have a combined native FAME, transport, timing-model
and generated-clock collateral boundary. Optional memory selection is still
absent from the default FireSim build, and full selected-memory Rocket runtime
verification remains pending. Legacy retained domain-clock annotations have
the same erased top-clock references as SFC after FAME; later consumers must
use captured domain identity. The next smallest step is optional selection
through the FireSim compiler pipeline, preserving this combined boundary and
letting the harness exercise its transformed RTL. Readwrite/wider memory shapes
and FAME-5 remain separate incomplete work.
