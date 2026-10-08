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

## Remaining scope

The single-instance SRAM model now has native FAME data/clock hardware.
Optional memory selection is still absent from the default FireSim build;
inter-model transport and abstract RAM timing-model replacement remain pending.
The next step is to generalize data-port rewriting to every promoted instance
of a shared SRAM definition, preserving each instance's distinct global
channel/clock targets while constructing the definition's local FSM once.
