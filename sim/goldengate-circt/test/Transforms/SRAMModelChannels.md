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
