# Fixed F2 SFC input/golden preparation

`GenerateF2SFCGoldens.py` runs target elaboration and the Scala Golden Gate
compiler directly, without Vivado. It validates exactly the four entries in
`automation/chia_firesim_circt/selected_config_build_recipes.yaml` against the
staging sample. Plain Shuttle must be the sample MegaBoom recipe with only
`TARGET_CONFIG` changed. No platform or target aliases are applied. In particular,
Saturn retains the sample's actual 60 MHz value.

The two assembly jars already archived under
`automation/chia_firesim_circt/golden/u250-sfc-20261013/toolchain/` contain the
four exact target configurations and F2 platform classes. Only these compiler
jars are reused; historical U250 inputs and results are never reused as F2.
Their pinned SHA-256 hashes are checked against the historical manifest. That
receipt records Chipyard revision `a4e4afff6794d1fee7cfbab00708535f80aeaf10`
and FireSim revision `5d8ebb741325ba12e5820e58d50088cf97f07310`.

Before generation the script requires current Scala/build/resource scopes to
match those source revisions, rejects untracked source changes, and checks all
35 relevant dependency checkouts against their recorded gitlinks. The current
HEAD revisions may differ because unrelated paths changed. This corroborates
the archived source receipt; it does **not** independently reproduce the jars
or prove that a jar was built from those sources. Source drift fails closed.
The excluded native compiler directory remains editable without altering this
Scala oracle identity. Missing live SBT assembly outputs do not block reuse of
the pinned archived jars.

Read-only prerequisite/argv check (also validates Java and jar contents):

```bash
cd /scratch/jfx/fsim-circt
python3 sims/firesim/sim/goldengate-circt/test/Support/GenerateF2SFCGoldens.py \
  --output-root /scratch/jfx/f2-sfc-goldens --target rocket --check-only
```

CHIA may run the actual Rocket preparation command after initializing the
required manager environment. Omit `--target rocket` to generate exactly the
four fixed targets sequentially. Each JVM uses the affinity core count by
default; `--threads` can reduce it, and `--heap-gb` defaults to 24.

```bash
source /home/ubuntu/miniforge3/etc/profile.d/conda.sh
cd /scratch/jfx/fsim-circt/sims/firesim
source ./sourceme-manager.sh
python3 sim/goldengate-circt/test/Support/GenerateF2SFCGoldens.py \
  --output-root /scratch/jfx/f2-sfc-goldens --target rocket
```

Each target gets a fresh, permanent `attempt-*` directory. Its manifest records
the recipe, source receipt/current revisions, Java launcher/runtime and jar
hashes, exact argv/cwd, thread/heap settings, original pre-MIDAS FIRRTL and
annotations, input sidecars, inline/classpath/file resources and boot ROMs,
all emitted SFC files, logs, and required blackbox concatenation. Raw SFC RTL is
retained alongside the concatenated simulator RTL. Absolute resource paths stay
valid because attempt directories are never renamed. External resource origin
and archived-copy mappings are recorded without editing input annotations.
The Java launcher, runtime image and VM library are hashed; optional `release`
metadata is recorded when present (the current Conda JDK omits it).
Source/dependency identity and every recorded tool/resource hash are rechecked
before publication. Required RTL, headers, XDC and annotation sidecars must be
nonempty. The peer-to-peer YAML must exist but may be empty, as allowed by Scala
when there are no peer-to-peer address routes.

Only a validated complete attempt publishes `<target>/complete.json`, binding
the immutable manifest by hash. Completed targets refuse regeneration in that
output root. Failed/interrupted attempts remain evidence and receive no marker;
a retry uses a fresh attempt and never treats partial files as inputs or goldens.
Consumers must require the marker, verify its manifest hash and all recorded
artifact hashes, and treat malformed/missing markers as incomplete. This script
does not populate CHIA's semantic adapters, claim compiler parity, or establish
syntax, proof, F2 runtime, or performance acceptance. Its one fresh JVM timing
is diagnostic; it is not a matched repeated speed benchmark.

Fast adapter tests, without compiler execution:

```bash
python3 sims/firesim/sim/goldengate-circt/test/Support/GenerateF2SFCGoldensTest.py
```
