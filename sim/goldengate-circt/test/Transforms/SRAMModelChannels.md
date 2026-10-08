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

## Remaining scope

Shared SRAM definitions now have native FAME data/clock hardware with distinct
instance bindings and native transport for complete scalar parent graphs.
Optional memory selection is still absent from the default FireSim build;
abstract RAM timing-model replacement and SRAM generated-clock collateral
integration remain pending. Legacy retained domain-clock annotations have the
same erased top-clock references as SFC after FAME; later consumers must use
captured domain identity rather than resolve those as surviving ports. The
next step is to canonicalize the eight equivalent parent output aliases at
model-port inference, then require complete parent ABI/completion equality
and compare every queue's payload and handshake equations.
