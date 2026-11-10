# TracerV retirement width support

2026-10-21, implementation iteration 2. This is focused native compiler
coverage, not four-target F2 acceptance evidence.

The Scala oracle is
`generators/firechip/goldengateimplementations/src/main/scala/TracerVBridge.scala`,
with payload definitions in
`generators/firechip/bridgeinterfaces/src/main/scala/TileTraceIO.scala`.
Archived iteration-262 U250 annotations show the actual previous failures:
Saturn Shuttle has two retirement lanes, while the BOOM fixture has three lanes
and optional 64-bit writeback data. These historical inputs establish constructor
shapes only; they are not substituted for the selected MegaBoom or F2 inputs.
The current MegaBoomV3 configuration uses four-wide decode in
`generators/boom/src/main/scala/v3/common/config-mixins.scala`.

`TracerVConfig.h`, `TracerVTokenEngine.cpp`, `TracerVTriggerConfig.cpp` and
`TracerVHeader.cpp` now support one through seven retirement lanes, optional
writeback data of width 1 through 64, and the selected common widths:
40-bit instruction address, 32-bit instruction, 64-bit cause and 40-bit tval.
Wider constructors are rejected before mutation. In particular, retirement
widths requiring multiple stream arms remain unsupported.

Every constructor-selected channel, including writeback data that is not
recorded in the trace stream, participates in the HostPort token join. Each
endpoint excludes its own valid/ready predicate, preserving the Scala helper
contract. The single 512-bit stream beat has the 64-bit cycle count in bits
63:0, then successive 64-bit `{maskedValid, paddedPC}` records starting with
lane zero. Target reset masks every lane's valid bit. Stream backpressure,
initialization, trace enable and trigger handling retain their existing roles.
Each lane owns separate PC/instruction trigger history updated on host clocks;
the selector ORs the lane states. Driver constructors retain the 6144-entry
queue and report the actual retirement width.

Missing optional `wdataWidth` and explicit JSON null both mean no writeback
channel. A direct compiler-import experiment under
`/scratch/jfx/goldengate-circt-f2-build/tracerv-null-input` confirmed that CIRCT
represents JSON null with `UnitAttr`. Other present noninteger widths, integer
attributes wider than 64 bits, incomplete channel schemas and duplicate
endpoints fail before mutation.

`lib/Support/AnnotationEmission.cpp` also serializes imported `UnitAttr` as JSON
null, allowing those constructors to survive retained-annotation export. The
real CLI test `goldengate-annotation-null-round-trip` passes, covering optional
constructor fields and nested null, string, boolean and integer values. This
does not resolve CIRCT's preexisting import ambiguity for the literal JSON
string `"null"`, which the importer also represents as `UnitAttr`.

Focused verification commands:

```text
cmake --build /scratch/jfx/goldengate-circt-f2-build --target goldengate-tracerv-token-test goldengate-tracerv-header-test --parallel 1
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build -R '^goldengate-tracerv-(token|header)$' --output-on-failure
```

Both focused targets built successfully. CTest passed 2/2: header 1.12 s,
token 155.43 s, total 156.55 s. Logs are
`/scratch/jfx/goldengate-circt-f2-build/tracerv-build.log`,
and `tracerv-ctest.log` in that build directory.
These are test execution durations, not compiler-speed measurements.
`git -C sims/firesim diff --check` also passed.

The token tests add retirement widths 2, 3, 4, 6 and 7, including 64-bit writeback
data at widths 3, 4 and 6. Per width they check 2048 packet/token cycles,
256 randomized channel joins, and 1024 per-lane PC/instruction history and
trigger-reduction cycles. Existing one-lane packet/next-state, independent
multi-bridge, trigger configuration, annotation identity and atomic-rejection
tests remain. The header test adds all five wider constructors, preserving
hardware while checking emitted retirement counts and shared stream indices.
No immutable SFC goldens were modified. No selected F2 transform comparison,
Jasper LEC/SEC, FPGA runtime or compiler-speed acceptance is claimed here.
