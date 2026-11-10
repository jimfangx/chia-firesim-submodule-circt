# Partial F2 comparison and paired syntax adapter

`CompareF2Semantics.py` is CHIA's per-target semantic adapter. It verifies
complete immutable SFC and candidate receipt/manifest bindings, complete emitted
inventories, the exact selected F2 recipe and the shared archived pre-MIDAS
FIRRTL/annotation identity before reading generated contracts. It does not
regenerate targets, run workloads or launch proofs.

The static comparison covers selected annotation records at bridge extraction
and inferred model ports (constructors, clocks, endpoints, optional fields,
ordered arrays and duplicates), F1Shim/FPGATop public ports, constructor arguments
and fixed-layout driver ABI assertions, TargetConfig, literal defines and
synthesis XDC, CPU stream address maps and the empty unpartitioned header.
Implementation XDC accepts only the generated-clock plus setup/hold multicycle
triplets emitted for these recipes. Every clock pin path is independently
resolved from `F1Shim` through actual named instances in each emitted RTL design
to the supported `BUFGCE/O` primitive. Missing modules, ambiguous instances,
unknown primitives/pins, duplicate clocks/paths, inconsistent triplets and
unsupported Tcl commands reject. Clock names, buffer identities, divide values
and setup/hold values are compared; snippet order and differing resolved parent
instance names may vary. No hierarchy spelling alias is accepted without actual
resolution. This checks emitted RTL only: the external F2 shell prefix,
pin persistence after synthesis/link and actual Vivado timing remain unverified.
Stream maps require unique names, exactly the three integer geometry fields,
nonnegative bases and positive capacities/widths. Mapping order, quoting and
spacing are normalized; duplicate keys, booleans and unknown fields reject.
The four selected unpartitioned headers may contain comments and whitespace
only. JSON record/dictionary order, public port order and equivalent
C++ spelling are normalized. String literal whitespace is preserved. Unsupported
parser forms fail closed.

CHIA also runs four actual syntax commands per target, sequentially within its
resource allocation:

- Verilator `--lint-only --timing --top-module F1Shim` on each complete finalized
  simulator RTL. Both use identical emitted platform defines and the same
  checked-in functional `midas/src/main/verilog/BUFGCE.v` simulation model. Every
  archived blackbox resource must appear byte-for-byte in the finalized RTL's
  checked raw-output/resource concatenation. No blackbox stubs or missing-module
  suppression are used. Warnings remain visible under `-Wno-fatal`; errors fail.
- C++20 `-fsyntax-only` on each complete generated `.const.h`, using real MIDAS,
  firechip bridge and RISC-V include directories. The probe exercises includes,
  fixed-layout checks, TargetConfig and all three constructor phases with the
  real widget registry and driver declarations, following `entry.cc` phases.

The evidence directory retains exact commands, tool versions and executable
hashes, logs, return codes, timeouts, wall times, input/resource/library hashes,
probe sources and driver include-header hashes. The selected launcher cannot
inherit a substituted Verilator backend or external compiler include override.
Timeouts terminate each command's process group. The adapter timeout in CHIA is
3000 seconds; individual RTL and driver checks allow 1200 and 180 seconds.

`syntax` becomes true only when both complete RTL and both complete driver
syntax commands, plus tool-version checks, return zero. This covers Verilator
simulation mode with the functional clock-buffer model; it does not prove the
synthesis primitive or behavioral equivalence. `passed` and `semantic` remain
**false**, even if syntax and every static contract match. Unresolved obligations
cover stateful RTL, memory behavior/initialization, clock/reset/token schedules,
MMIO/backpressure, post-synthesis/link XDC objects and timing, and remaining annotation
and sidecar contracts. An inventory check establishes integrity, not semantics.
CHIA therefore cannot release downstream nodes from this partial adapter. Zero
adapter process status means a fresh comparison report was emitted; CHIA rejects
false acceptance fields.

Run focused tests without full-target jobs:

```
.conda-env/bin/python3 sims/firesim/sim/goldengate-circt/test/Support/CompareF2SemanticsTest.py
```

Iteration 13's seventeen tests include tiny real Verilator/C++ success and
failure fixtures, an unresolved-blackbox rejection, the multi-include probe
against actual framework declarations, exact resource-append validation,
command/log evidence and timeout handling. No full-target syntax or driver job
was run during adapter implementation. CHIA must supply those actual results.

Iteration 14 adds two tests for normalized stream metadata and unpartitioned
headers, bringing the focused total to nineteen. Archived candidates omitted
both sidecars, so the expanded comparison requires freshly emitted candidates.
See `../F2RuntimeCollateral.md` for the compiler fix and its component scope.

Iteration 15 adds four focused generated-clock comparison tests, bringing the
total to twenty-three. They cover independently resolved hierarchy changes,
snippet ordering, missing/ambiguous RTL objects, primitive pins, unsupported
commands and changed clock parameters. Full semantic acceptance remains false.

Iteration 12 archived static evidence remains at
`/scratch/jfx/goldengate-circt-f2-build/iteration12-semantic-adapter-archived-all4.json`.
It concerns the earlier candidates, not fresh validation of a later compiler.

Fresh iteration 13 static evidence is
`/scratch/jfx/goldengate-circt-f2-build/iteration13-semantic-static-all4.json`.
It binds all four iteration-12-generated candidates and immutable SFC manifests.
All measured static boundaries now match, including the model/channel records
corrected by the baseline optional-SRAM fix. Every result still has false
acceptance fields; the read-only audit does not invoke the paired syntax tools.
The separate `iteration13-clock-state-audit.json`, `iteration13-reset-bridge-probe/`
and `iteration13-register-file-probe/` record precisely scoped emitted-RTL
component evidence. They do not establish whole-target equivalence.

Driver include-header hashes are retained in each syntax result. Changes in
external include trees still need comprehensive CHIA identity coverage before
these reports could contribute to final acceptance; current reports cannot
release downstream nodes.
