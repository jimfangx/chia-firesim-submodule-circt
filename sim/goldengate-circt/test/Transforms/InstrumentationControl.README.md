# Enabled CSV and Print shared control composition

`compileInstrumentationControl` consumes post-FAME FIRRTL and retained CSV and
Print constructors. It materializes the actual queued hosts, binds their channels,
and registers all CSV banks followed by all Print banks and the CPU-managed
stream engine. This follows `FPGATop` registration order within this selected
catalog. Allocation uses actual register counts and `HasWidgets` stable size
sorting. The same allocation drives the global decoder and emitted driver header.

The output has one AXI control slave and one complete CPU-managed AXI slave.
The CPU transport includes independent depth-two AW/W/AR/R/B queues and the
real zero-sink write engine. Print keeps its 6144x512 queue, width adaptation,
accepted-cycle ROI, global-reset suppression, idle encoding and narrow flush.
Other bridge constructors reject instead of disappearing from the allocation.
All public composition APIs clone, validate, verify and commit atomically;
rejections preserve caller IR, child handles and header/decoder/address-map text.

The CLI is `--synthesize-instrumentation-control`. Input fixture
`InstrumentationControl.fir` plus its `.anno.json` enables two independent CSV
widgets and one two-record Print widget. F2 has 32-bit control addresses; the
unit also covers the 25-bit control width of the existing implementation. The
fixture is a compiler handoff seam with explicit external target tokens. It is
not an elaborated Linux platform.

`InstrumentationControlOracle.scala` calls unchanged production
`AutoCounterBridgeModule`, `PrintBridgeModule`, `CPUManagedStreamEngine`,
`HostPortIOConnectChannels2Port`, `HasWidgets.genCtrlIO`, `AXI4Buffer`, and
`genWidgetHeaders`. Its serialized original constructors/channels must exactly
match the native fixture. The behavioral differential compares every visible
control handshake/response and each complete 512-bit DMA beat, normalizes only
hierarchy retargeting in the retained annotations, and varies initial randomized
state independently on the two implementations. The actual driver constructor
comparison compiles both emitted headers against unchanged C++ types and checks
CSV addresses/permissions/event vectors, Print register order/format and argument
offsets/widths/token mask/clock, and CPU descriptors. The mutation control flips
one signed Print argument bit in emitted packing logic; it compiles and runs
successfully but must fail the full-network semantic comparison.

Checkpoint38 evidence is under
`/scratch/jfx/goldengate-circt-f2-build/iteration38-feature-composition/instrumentation`.
The matched oracle has exactly two CSV banks, one enabled Print bank and one CPU
count bank, at byte bases 0, 64, 128 and 160. The cycle comparison covers 5347
cycles, 2621 accepted Print bags and 27 DMA beats, plus 723 control reads, 39 samples
per CSV widget, live Print/count-register readback, ROI start/end crossing,
independent trigger/reset and channel stalls, pipelined reused
control IDs, and unmapped burst DECERR. Absolute addresses come from the complete
selected enabled oracle, not a Print-disabled baseline or guessed placeholder.

Remaining integration includes the source PrintSynthesis + CSV/FAME model/clock
pipeline, original SimulationMaster/memory/platform bridge catalogs and shell,
feature-enabled representative execution, separate Jasper LEC and SEC, real F2
Linux checked counter/Print output and clean poweroff. These focused results do
not populate feature-evidence receipts or establish full feature acceptance.
