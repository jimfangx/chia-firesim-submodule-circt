# Memory and combinational virtual models with exported clock aliases

`FAMEModelGraphMemory.fir` and `.anno.json` are one fixed prepared input used by
both implementations. The old native compiler rejects it with `virtual models
with exported clock aliases are unsupported`. The unchanged SFC transform
accepts it. The graph rewrite now retains each virtual model's exported clock
alias as a local wire, using the same existing clock-output transfer as the hub.
This case originally admitted only unique definitions with exported aliases.
The subsequent `FAMEModelGraphRepeatedAliases` case now covers the generalized
all-instance transfer for repeated definitions; see
`FAMEModelGraphRepeatedAliases.md` for its exact supported wiring and checks.

The case has a two-domain hub (rational clocks 1/2 and 1/3), a virtual memory model
containing a 16x8 asynchronous-read memory and a 16x8 synchronous-read old-RUW
memory, and a separate virtual combinational XOR model. Both virtual models
export a clock alias used by their channel-clock annotations. Memory write
enable, mask, address and data remain target signals; no target memory clearing
or initialization is introduced. All channels are ordinary scalar Pipe channels.

`FAMEModelGraphMemoryOracle.scala` invokes the unchanged production
`FAMETransform`, `AddRemainingFanoutAnnotations`, `DefineAbstractClockGate` and
`SimWrapper` from the immutable SFC jar. It retains the exact pre/post-transform
FIRRTL and annotations. For boundary RTL it sends the same transformed circuit
to the unchanged Scala `VerilogCompiler`. Only emitter-side annotations are
omitted at this scoped boundary: the retained XDC annotation marks an instance
field as a DontTouch target that the standalone Verilog compiler cannot resolve.
The complete original annotations are independently compared without this
omission, including both generated-clock gate/name/MFMR contracts. Neither SFC
production sources nor immutable goldens are modified.

The initial firtool-only SFC emission was inadequate for this case: its default
memory lowering registered the synchronous read address and returned the new
value on a write collision, despite the old-RUW descriptor. The actual Scala
Verilog compiler registers read data and matches the native compiler. The first
failed attempt and its monitor assumptions are retained in
`first-firtool-oracle-ledger-failure/`; collision checks were not waived.

`FAMEModelGraphMemoryCompare.scala` parses independently emitted FIRRTL and
compares complete model/top channel interfaces, complete top wiring, both target
memory descriptors, every memory control/data/clock connection, and the clock
alias wires. No native output is transformed using SFC.

`FAMEModelGraphMemoryCompare.py` runs actual native CLI and actual SFC RTL with
independent Verilator startup seeds 11/73 and the unchanged registered SFC clock
gate. Stable producer tokens, independent channel stalls, held rational clock
tokens and three host resets exercise 16,384 host cycles. Ready/valid/clock
controls are strict after the first reset edge. Hub register payload is compared
after explicit reset-token warmup. An independent memory ledger monitors actual
gated-clock edges, checks asynchronous reads only at written addresses, checks
synchronous old-RUW reads when their sampled address was written, and persists
across host resets. Monitoring state never drives target state. The gate's
unreset startup enable may produce an initial edge; only the monitor's initial
edge-counter offset is normalized, with later edge schedules compared strictly.
The XOR result is checked independently. Invalid memory payload perturbation
passes, while valid memory corruption and input-ready corruption fail.

The unit test additionally covers three atomic malformed cases (repeated alias
instance, unknown data-port annotation, and a foreign clock target), both
boundary/transport modes, memory preservation and combinational target state.
The actual unchanged SFC SimWrapper and full native Pipe wrapper also emit and
pass syntax checks. The runtime comparison here covers their complete FAME
boundaries; it does not establish transported-wrapper RTL equivalence, platform
MMIO/driver ABI, Jasper LEC/SEC, Linux/F2 feature execution, or full feature
acceptance. The separate unchanged internal-RV SimWrapper defect remains open.
