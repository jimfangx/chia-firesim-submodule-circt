# CHIA-owned F2 CIRCT artifact preparation

`GenerateF2CIRCTCandidates.py` compiles the four fixed archived pre-MIDAS
FIRRTL/annotation pairs through the real native `--compile-baseline` pipeline.
CHIA invokes it after building the configured native compiler. It performs no
build, target elaboration, SFC fallback, Vivado invocation, proof or runtime test.
A complete receipt establishes artifact preparation only; the independent
syntax/semantic adapter must still compare the complete designs and collateral.

Example invocation for the CHIA preparation command:

```sh
python3 sims/firesim/sim/goldengate-circt/test/Support/GenerateF2CIRCTCandidates.py \
  --repo /scratch/jfx/fsim-circt \
  --golden-root /scratch/jfx/f2-sfc-goldens \
  --output-root /scratch/jfx/f2-circt-candidates \
  --compiler /scratch/jfx/goldengate-circt-f2-release-build/goldengate-circt \
  --threads 16
```

Omitting `--target` selects exactly Rocket, MegaBoomV3, plain Shuttle and Saturn
Shuttle. Repeated `--target` arguments select only missing/stale members of that
fixed set. The selected YAML is checked against the staging sample, including
plain Shuttle's documented change of only `TARGET_CONFIG` from the exact
MegaBoom recipe. Platform fragments remain intact. Every requested target is
attempted even if a different target fails; `generation-result.json` records the
per-target result and the process returns nonzero for any failure. `--check-only`
validates prerequisites and reports errors without writing candidate outputs.

The helper reuses `verification_nodes._sfc_receipt` to validate the immutable SFC
marker, manifest, full inventories, exact recipe, archived compiler jars,
pre-MIDAS pair, boot ROM mappings and original filesystem resources. It invokes
the native compiler directly on those same archived inputs, with its working
directory matching the real compiler make rule. It records the exact argv,
compiler hash, selected/sample recipe hashes, original input/resource hashes,
production native source inventory (including new and removed files), direct
make/CMake wiring and helper/gate sources. Native compiler processes inherit a
CPU affinity capped at the requested available cores. The compiler's MLIR
threading remains at its native default; the helper does not claim an observed
thread count or disable MLIR threading.

Every attempt uses a fresh directory. The helper requires primary simulator
RTL, both headers, defines, both XDC sidecars, bridge-extraction annotations and
final XDC annotations. Defines/XDC files may legitimately be empty; their
presence and content remain bound. All actual emitted files, including optional
IP-generation Tcl, annotations and resource files, are recursively inventoried.
Symlinked output files are rejected. It retains `.raw.sv`, then appends the
blackbox file list into final `.sv`, matching the production make rule. External
resources are copied beside the candidate and their original and archived bytes
are checked. No inputs or SFC archives are modified.

Only successful exit, complete required outputs, valid JSON, unchanged input /
compiler / source state and a complete output inventory permit `completion.json`.
The target's `complete.json` pointer is replaced atomically after that point;
older attempts remain intact. Failed attempts retain partial inventories,
commands, compiler logs and diagnostics without a successful marker. Existing
complete receipts are reused only when the entire current provenance identity
matches. Changed provenance produces a new attempt. Missing, additional or
modified current output files invalidate a complete receipt and raise an error.
This reuse is useful for correctness preparation and provides no speed result.
Recorded process time, combined compile/resource-append time and cumulative
children peak RSS describe one preparation attempt, without matched repetitions
or compiler-speed acceptance claims.

The import API used by CHIA is:

```python
validate_receipt(repo, golden_root, output_root, compiler, target, threads)
```

It returns `None` for a missing/stale candidate, or a dictionary containing
`circt_artifacts`, `allowed_empty`, `manifest`, `input_identity`, `target` and
`verification_claim`. Corrupt receipts or changed current output files raise.
The artifact list includes every output, completion metadata and provenance file,
so gate identity changes when any consumed or generated content changes.

Focused contract checks use small synthetic SFC receipts validated by the real
receipt checker and a fake native compiler subprocess:

```sh
python3 sims/firesim/sim/goldengate-circt/test/Support/GenerateF2CIRCTCandidatesTest.py
```

They exercise all four exact commands, unchanged SFC archives, complete inventory,
blackbox append, legitimate empty sidecars, source membership/content and binary
staleness, output corruption, failed and partial attempts, immutable old attempts,
resource drift, platform substitution, missing markers, invalid CPU counts,
overlapping roots, continue-after-failure and valid-receipt reuse. These tests
never compile a real target and cannot establish semantic parity.
