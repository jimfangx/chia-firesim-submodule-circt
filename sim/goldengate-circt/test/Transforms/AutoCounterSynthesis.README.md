# Enabled CSV target synthesis supporting case

`synthesizeEnabledAutoCounterCSV` implements the enabled CSV branch of the
unchanged SFC `AutoCounterTransform`: select manual and enabled cover events,
gate each event during its local reset, export every hierarchy instance, resolve
source clocks, group the exported events by clock, and emit typed CSV bridge
constructors, outgoing pipe channels, trigger sinks and global reset sinks.
The transform consumes AutoCounter selection annotations. It stages all changes
on a clone and commits only after FIRRTL verification; a failure preserves the
original operations, SSA values, annotations and result count.

The actual compiler boundary is:

```
goldengate-circt AutoCounterSynthesis.fir \
  --annotation-file AutoCounterSynthesis.anno.json \
  --output-dir output --synthesize-autocounter-csv
```

This boundary requires LowForm input after LowerTypes/ExpandWhens, matching the
input of the SFC transform. It does not run the general retained-target
normalizer: that normalizer resolves every retained cover event before selection
and would incorrectly reject the intentionally excluded malformed cover in this
fixture. Production staging must filter excluded generated covers before strict
retained-target normalization. The optional
`output/autocounter-covermodules.txt` uses exact module names and Scala `getLines`
semantics; unknown names do not select events. Missing operation metadata uses
the SFC annotation default Accumulate. An explicitly present empty operation
object is ambiguous and rejected.

The fixture instantiates the same two-level hierarchy under two independent
clock/reset domains. It exercises UInt8 accumulation inputs, UInt64 identity
inputs, a UInt4 top-level event, cover selection, a clock alias, namespace
collisions, and excluded malformed cover operands. Four selected source
annotations produce seven event exports and two CSV constructors. The original
target outputs remain unchanged. The unit also checks nine malformed-input
failures, including a late channel-name collision after staged hierarchy edits.

`AutoCounterSynthesisOracle.scala` runs the unchanged production CSV transform
and `ResolveAndCheck` on those exact files. Its supplemental serialization hint
annotation preserves actual EventMetadata/Accumulate/Identity singleton types
at the standalone JSON export boundary; it neither changes transform behavior
nor infers an operation from an empty object. `AutoCounterSynthesisCompare.py`
compares complete normalized annotations, actual emitted interfaces and 8192
samples of native and SFC emitted RTL using independently changing clocks,
resets and payloads. All 15 outputs are defined, including default trigger and
global reset predicates; there are no memories or zero-out policy changes.

The SFC top-wiring transform builds its event constructor list by traversing
Scala 2.13.10 immutable hash maps: first ComponentName source groups and then
pathless ReferenceTarget keys. Native synthesis implements the corresponding
case-class product hashes, Java UTF-16 string hashes, hash improvement and CHAMP
payload-before-child traversal. It preserves instance order within each source
group and collision-node insertion order. This fixes the prior positional
event permutation before the CSV host assigns event MMIO registers. The
comparison now requires every constructor event position to match, without
sorting eventMetadata or renumbering counters in the comparison.

`AutoCounterSynthesisOrdering.py` exercises 1, 4, 5, 40 and 128 source keys,
two deliberate full-hash-collision insertion orders, and colliding module names
with repeated instances under independent clock domains. It executes the actual
compiler and unchanged Scala transform on each exact generated input, comparing
all constructor and annotation fields without event-order normalization and
checking SFC RTL syntax. These bounded source-level cases exercise the map-size
boundary and nested hash-trie branches rather than enumerating configurations.
The original focused RTL case continues to compare all defined outputs.

The composed compiler mode `--synthesize-autocounter-csv-hosts` performs this
target synthesis and then calls the existing actual CSV host materializer on
those newly generated constructors. It emits the target RTL and separately
rooted `post-autocounter-csv-host-N.sv` files alongside the complete IR and
annotations. The hosts retain local F2 AXI32/id12 controls. Scalar target outputs
are not represented as host tokens: FAME/token binding remains a subsequent
compiler stage and the diagnostic hosts are deliberately separate circuit roots.

`AutoCounterSynthesisHostOracle.scala` takes the exact same source files through
the unchanged target transform and directly elaborates each resulting typed
constructor with the actual SFC AutoCounterBridgeModule. Its local F2 widgets
use explicit base1/1 metadata; register geometry does not depend on that ratio.
`AutoCounterSynthesisHostsCompare.py` checks the native compiler-emitted bank
register labels/addresses against the SFC registry, then compares both actual
emitted host RTL arms under accepted-token stalls, trigger/global/host reset,
periodic sampling, queue-full sample drops and backpressured AXI MMIO reads and
writes. It uses distinct randomized initial-state seeds and an asserted reset
edge before observations. Unreset sample slots are read only after a valid
enqueue and countersready; no memory preclear or zero-out change is used.

This compiler stage does not yet compose trigger/global reset/clock wiring,
FAME tokenization, the existing CSV hosts, global MMIO/header allocation and the
complete F2 platform into one enabled pipeline. Complete Print integration and
feature-enabled semantic/LEC/SEC/F2 Linux output checks also remain required.
These focused receipts cannot satisfy the required feature acceptance guard.
