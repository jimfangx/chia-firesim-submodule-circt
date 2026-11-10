# FAME5 target memories

`threadFAME5Memories` implements both address mappings of the existing
SFC `ImplementThreadedMems`: an original depth-D memory becomes depth D*N, and
power-of-two counts use `{originalAddress, activeThread}` and other counts use
per-accessor rotating base addresses. The pass retains
each port's data/mask subtree, clock, enable, write mode, read-under-write
attribute, annotations that do not require a rename, and original read/write
latencies. Register-state banks from `threadFAME5Registers` are skipped.

The supported boundary has zero/one-cycle reads and one-cycle writes, including
read-only, write-only and readwriter ports. Target depths need not be powers of
two. One-cycle reads use SFC's per-thread response buffers described below.
Non-power-of-two counts require a canonical hostClock input. Initialization files,
annotation targets requiring a rename, debug ports and noncanonical address
connections reject atomically. The caller supplies the active index and owns model scheduling.

The focused fixture has a depth-three `{lo: UInt<8>, hi: UInt<8>}` memory,
independent byte masks, one reader, one writer and one readwriter. The Scala
oracle invokes unchanged `MuxingMultiThreader.onStmt` followed by unchanged
`ImplementThreadedMems` and the existing SFC Verilog compiler. CIRCT replays the
exact same saved input files. No archived SFC golden is changed.

Supporting checks include FIRRTL verification, expanded geometry/address mapping,
mask/data/control type preservation and seven atomic rejections. Both exported
RTL arms run `FAME5ThreadedMemoryRTL.cpp`: initialize all legal words through
ordinary writes, then execute 8,192 selected-thread cycles with independent
masked writer/readwriter stores, readwriter reads, disabled ports and stalled
target clocks while host clocks continue. Every thread's three words are
observed after every edge; unspecified initial or disabled-read values are
excluded. Multiple writer ports use distinct addresses to avoid undefined
simultaneous writes to the same word.

The focused evidence directory is
`/scratch/jfx/fame5-threaded-memory-20261024/`. Its receipt binds input, source,
compiler, RTL and test hashes, and records the commands and observed checksums.
Both independent RTL arms pass with exactly the same observations: two threads
produce 49,152 observations and checksum `17289719672077809564`; four threads
produce 98,304 observations and checksum `4990882079842146862`.
This receipt supports the combinational-read banking slice. The separate
`FAME5GroupedModels.md` records bounded grouping and static scheduling support;
enabled production threaded Linux execution and separate Jasper LEC/SEC remain
required before FAME5 feature acceptance.

## Synchronous-read response banking

For one-cycle target reads, the implementation additionally follows unchanged
SFC `ImplementThreadedMems`' host-clock schedule. A toggle on the shared target
clock and a host-clock tracker detect target edges. Each reader and readwriter
gets a distinct depth-N response memory. Target-edge enables and thread tags
travel alongside read data; response banks are written only after a target edge.
For two threads, response-memory read latency and the data pipeline depth are
zero. For four or more threads, both are one. The source target memory keeps its
read latency, write latency, geometry, masks and read-under-write behavior.

This branch requires scalar lowered UInt/SInt data, a `hostClock` input, a single
shared input clock or supported always-enabled `AbstractClockGate` output for all
memory accessors, and one unconditional clock/address connection per port. Clock
nodes and acyclic uniquely driven clock wires resolve to their shared root. The
gate must have the exact unchanged SFC external ABI, its I input must resolve to
hostClock and CE to UInt<1>(1). Dynamic gate enables and arbitrary generated
clocks reject until their complete token-staging relation is integrated. Aggregate
memories can be lowered before invoking this API; the memory CLI does this.
Differing accessor clocks and higher read latencies
reject before any mutation. The
helper registers deliberately have no reset, matching SFC. The caller must
provide a free-running round-robin thread index and establish startup alignment.
This primitive does not prove those scheduling preconditions for its caller.

`FAME5SynchronousMemory.fir` supplies a scalar API fixture. The memory unit checks
independent reader/readwriter response banks, their geometry and host/target
clock ownership for two/three/four threads and both signed/unsigned data, plus
six atomic clock/latency/alias rejections.
The behavioral comparison instead uses the original aggregate masked-memory
fixture with `read-latency => 1`, saved identically for both compilers. The SFC
oracle invokes its unchanged threading and memory implementation passes, then
its Verilog compiler. `FAME5SynchronousMemoryRTL.cpp` checks each selected thread
against an independent ordinary-memory model for 16,384 cycles. It covers
distinct masked writer/readwriter stores, previously issued synchronous reads,
disabled read ports and target-clock stalls while the host scheduler advances.
Periodic same-address reader/write-port collisions check `read-under-write => old`
against pre-edge memory contents, while the two writer ports use distinct words.
All legal words are initialized through ordinary writes; startup pipelines are
warmed, disabled-read/write-mode responses are excluded, and simultaneous
writers use distinct addresses. These are explicit test assumptions, not a reset
or initialization proof.

The supporting receipt is
`/scratch/jfx/fame5-synchronous-memory-20261025/comparison.json`. Its source,
input, oracle, compiler, RTL and scoreboard hashes bind this focused comparison.
The reproduction commands and all native/SFC build and run logs are retained
beside that receipt. Enabled production representative semantics, LEC, SEC and
F2 Linux runtime remain separate incomplete obligations.

The collision check also exercises the shared RTL backend normalization for
mixed reader/writer/readwriter memories. Such memories now split each RW port
while retaining global read latency one; the existing reader normalizer then
captures every original and split reader before switching storage to latency
zero. This preserves old-data collision semantics without changing the existing
sole-RW normalization or the guards for new-data RUW, annotations and symbols.

Native reproduction:

```sh
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build \
  -R '^goldengate-fame5-threaded-(state|memory)$' --output-on-failure
/scratch/jfx/goldengate-circt-f2-build/goldengate-circt \
  sims/firesim/sim/goldengate-circt/test/Transforms/FAME5ThreadedMemory.fir \
  --annotation-file sims/firesim/sim/goldengate-circt/test/Transforms/FAME5ThreadedMemory.anno.json \
  --output-dir /scratch/jfx/fame5-threaded-memory-20261024/native4 \
  --thread-fame5-memories Model 4 threadIdx
```

Compile `FAME5ThreadedMemoryOracle.scala` with Scala 2.13.10 and the existing
MIDAS classes/dependency classpath, then run
`midas.passes.fame.FAME5ThreadedMemoryOracle input.fir 4 output-directory`.
Build the emitted SFC and CIRCT RTL separately with Verilator `--cc --exe
--build --top-module Model` plus `FAME5ThreadedMemoryRTL.cpp`, and invoke each
generated executable with the same thread count.


## Non-power-of-two accessor schedules (iteration 39)

The memory API now admits non-power-of-two thread counts with the unchanged
SFC `ImplementThreadedMems` mapping. Each original accessor retains its own
unreset host-clock base-address register, rotating by the original memory depth
from zero through `(threads - 1) * depth`. The translated address is its base
plus the original address, with FIRRTL destination-width truncation. Power-of-two
counts retain `{address, thread}` interleaving. The compiler adds no reset to
these counters and performs no memory initialization. A missing hostClock input
still rejects before mutation.

The memory unit checks every legal address and base-counter transition for
three threads, including aggregate masks and a reader/writer/readwriter memory.
The synchronous reader/readwriter case also checks the three-thread target-edge
and response-bank schedule. `FAME5HierarchyCompare.py` covers complete unchanged
SFC recursive grouping for two, three and four threads with independent state,
per-sibling written-memory ledgers, stalls and reset. Three-thread simulation
explicitly starts each accessor base counter and module-local selector at zero;
this bounded startup relation does not establish an arbitrary-X formal proof.
The midtraffic host reset spans a whole rotation and leaves target storage and
base counters intact. Supporting results belong in the iteration-39 receipt;
separate Jasper LEC/SEC and an enabled F2 Linux representative remain required.

The three-thread comparison also retains an SFC reference limitation: asserting
hostReset at cycle 8192, when the local helpers are at slot two, and holding it
for three cycles resets only the top arbiter to zero. The unchanged SFC async
hierarchy first disagrees with the persistent logical-thread ledger at cycle
8192; its synchronous counterpart first disagrees at cycle 8195. Existing
`SimWrapper` directly connects hostReset to its reset input, and neither the
SFC grouping counter nor the local-helper code enforces a phase-aligned reset
release. No production guarantee of that alignment has been established. The
passing bounded runs use cycle `8192 - (8192 % threads)` and a complete thread
rotation under reset, preserving the startup phase; they do not demonstrate
arbitrary host-reset correctness. Initial helper assignments occur only at test
startup, on both independently emitted RTL arms. No assignments to base counters
are made during the midtraffic reset. The failing generated ledgers and logs are
retained under `iteration39-fame5-nonpower/comparison-fixed/threads3/sfc/` and
`iteration39-fame5-nonpower/synchronous/comparison/threads3/sfc/`.


## Signed synchronous memories in dynamic hierarchies (iteration 40)

The synchronous-memory API preserves scalar signed data and response types.
The prior UInt-only preflight rejected an SFC-supported SInt memory even though
its response implementation already retains the memory's type. The first nested
comparison exposed a second UInt-only guard in the simulator backend: signed
old-data RAM bypassed data capture and returned post-write collision data. The
backend now uses its same synchronous reader/readwriter capture for scalar
integers, retaining the New-RUW and unsupported-shape guards. The changed
threading preflight admits known-width scalar integers and preserves the existing clock,
mask, readwriter, response latency, RUW and annotation checks. It does not lower
aggregate signed memories or change helper resets.

`FAME5DynamicHierarchySignedMemory.fir` exercises a canonical virtual-clock
Worker containing Branch/Leaf descendants. Leaf owns a masked signed depth-four
latency-one RAM; its writes sign-extend five-bit input values, so both negative
and positive target data traverse the response banks. The same fixed input and
annotations pass through unchanged SFC grouping, its Xilinx specialization and
its Verilog compiler, and through native grouping with explicit Xilinx clock
semantics. `FAME5DynamicSynchronousMemoryCompare.py --nested-signed` checks the
complete public interfaces and written-memory ledger for two/four threads with
real BUFGCE semantics, independent stalls, disabled reads and old-data
collisions. It starts the used module selectors with the documented SFC phase
relation, checks all four Boolean edge-helper startups, and compares only
written/qualified response data. No RAM or target state is precleared.

Compiled wrong-mask and wrong-sign-extension inputs must both fail the
independent ledger. Ready/valid and written-response faults must fail; invalid
payload perturbations must leave the qualified checksum unchanged. These
supporting checks preserve the existing arbitrary-host-reset/startup gap and do
not establish production threaded Linux, full-platform composition, Jasper LEC
or SEC, or enabled F2 runtime acceptance.
