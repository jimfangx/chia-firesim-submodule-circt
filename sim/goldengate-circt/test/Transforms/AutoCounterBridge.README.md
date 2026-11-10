# CSV AutoCounter widget supporting case

`AutoCounterBridge.fir` and `.anno.json` are the small constructor-only input
emitted by `AutoCounterBridgeOracle.scala`. The fixture deliberately does not
claim CSV target synthesis, channel/token binding, global platform allocation,
driver construction, or the required enabled Print+AutoCounter Linux case. Its
synthetic `autocounter0` annotation target is retained until that later binding.

The unchanged SFC `AutoCounterBridgeModule` elaborates two real 64-bit events:
Accumulate and Identity. Its complete widget RTL and MMIO map are the semantic
oracle for the native CSV widget. `AutoCounterBridgeCompare.py` lowers and runs
both widgets through real AXI transactions, comparing visible signals each
cycle. It exercises delayed periods 0/3/17, accumulator rollover, triggers,
global target reset, host reset, target stalls, full sample-queue drops,
independent AW/W arrival and R/B backpressure. The actual compiler-emitted RTL
can be supplied with `--native-rtl`; no native helper RTL adapter is needed.

Pre-MIDAS `AutoCounterFirrtlAnnotation` already preserves its concrete operation
through `HasSerializationHints`. Standalone export of a synthesized
`BridgeIOAnnotation` with an `AutoCounterParameters` key lacks nested operation
hints and serializes both operation singletons as `{}`. The supporting oracle
adds a separate `ConstructorHints` annotation solely for JSON serialization;
it serializes the actual Scala singleton classes without guessing operations.
Native materialization rejects untyped `{}` operations atomically. Production
CSV synthesis must derive its constructors from the already typed pre-MIDAS
counter annotations.

The raw annotations remain in the native compiler snapshot. The standalone
FIRRTL lowering check consumes them from a copy after interface/constructor
validation, matching the compiler RTL emission boundary. This supporting case
has no Jasper proof or F2 Linux receipt and cannot satisfy feature acceptance.
