# Complete transported multi-domain model graph RTL

The fixed `FAMEModelGraphTransport` fixture has one two-domain stateful hub,
two independent instances of a shared scalar-clock Worker, internal atomic
fanout and seven Pipe queues, including a latency-one initial zero token.
It uses rational clock-token schedules 1/2 and 1/3. Unlike the earlier simple
`FAMEModelGraph` fixture, its scalar local channel names match scalar port names.
The earlier local input/output/other names differ from data/out/otherOut;
actual unchanged Scala VerilogCompiler rejects that earlier FAME target because
SFC top wiring names nonexistent scalar data_sink/out_source ports. Earlier
structural and interpreter evidence is scoped accordingly. The new fixture
preserves the topology while allowing real unchanged SFC target RTL emission.

Generate actual target and wrapper through `FAMEModelGraphMemoryOracle.scala`
on this fixture. Generate native output through `--rewrite-fame-model-graph`.
Then run:

```
python3 FAMEModelGraphTransportRTL.py EVIDENCE_DIRECTORY
```

The script links the actual Scala VerilogCompiler target to the unchanged SFC
SimWrapper through a named-port-only TargetBox adapter. The adapter maps clock
vector index0 to c0 and index1 to c1. It links both sides to the actual unchanged
SFC registered gate, rather than a combinational gate surrogate. The native
complete Pipe wrapper remains intact. No target state is forced or initialized.

Over 32,768 cycles it compares all external ready/valid controls and all actual
target-clock edge schedules after the first host reset edge. Both data producers
and the clock producer hold valid tokens until accepted. Consumer stalls are
independent, and three host resets occur without clearing target state. Read-only
monitors require all four actual register domains to receive their own explicit
target reset by cycle1024. All subsequent valid external payloads compare
strictly. The gate's possible startup edge counter offset is normalized without
changing the DUT. Actual clock-lane swap, output-ready crosswire and removal
of the latency-one initialization token must fail.

The retained annotations and two generated gate/name/MFMR contracts compare
separately on the matched FAME boundary. Final compiler binding checks require
identical emitted full-wrapper RTL, MLIR and annotations to the executed output.
This is bounded two-state, prepared-graph compiler support evidence. Complete
pre-MIDAS extraction, platform/driver assembly, memory transported graphs,
separate Jasper LEC/SEC and enabled Linux/F2 feature acceptance remain open.
