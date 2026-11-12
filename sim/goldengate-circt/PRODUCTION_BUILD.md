# FireSim production compilation

FireSim uses the native CIRCT compiler for normal builds. The manager's
`replace-rtl` step generates RTL and builds the driver; its subsequent `driver`
step reuses that compilation when inputs and compiler settings are unchanged.
No SFC compiler is run by this path.

The Firechip frontend and native compiler bootstrap have independent inputs.
The Firechip build rule starts them together, waits for both, and only then
allows design compilation. The compiler bootstrap uses the manager environment;
the frontend sources Chipyard's `env.sh` as before. Failure or interruption
terminates the supervisor's owned process trees and waits for its children.
This does not enable parallel execution of unrelated top-level Make rules.
On Linux the supervisor also adopts and reaps orphaned descendants, including
Ninja workers in separate process groups. The same supervision protects direct
bootstrap and design-compiler calls, so cancellation completes before their
build/output locks are released. SIGINT, SIGTERM and SIGHUP are handled.

## Native compiler build

Production recipes in one checkout share an incremental CMake/Ninja build.
Its configuration includes the source and CIRCT locations, selected C/C++
compiler, build type and native compiler flags. A file lock serializes CMake
configuration and build operations for each configuration. Each successful
build creates an immutable executable named by its SHA-256 beside the original
executable, preserving relative runtime-library paths. Recipes can then run
their pinned executables concurrently without racing a later compiler rebuild.
An interrupted bootstrap is reconfigured and cleaned before reuse.
The cache also fingerprints installed CIRCT/LLVM/MLIR headers, CMake package
files and libraries using file metadata, including inode and change time.
Replacing an installation at the same prefix invalidates its cache even when
file modification times are preserved; library bodies are not rehashed on each
invocation. Only the fingerprint and file count enter per-design receipts.

These ordinary Make overrides control the native build:

| Variable | Default or purpose |
| --- | --- |
| `GOLDENGATE_CIRCT_BUILD_ROOT` | `sim/generated-src/goldengate-circt-build` in this checkout |
| `GOLDENGATE_CIRCT_BUILD_TYPE` | `Release` |
| `GOLDENGATE_CIRCT_BUILD_JOBS` | All available logical CPUs |
| `GOLDENGATE_CIRCT_PREFIX` | CIRCT installation inferred from `firtool` |
| `GOLDENGATE_CIRCT_CXXFLAGS` | Additional flags for building the compiler itself |
| `GOLDENGATE_CIRCT_LDFLAGS` | Additional executable-linker flags for the compiler |
| `GOLDENGATE_CIRCT_PYTHON` | `python3` |

Driver targets export their own `CXXFLAGS` and `LDFLAGS` to prerequisites.
Those flags are excluded from native CMake configuration so a separate
`make driver` does not select another compiler build or inject driver SDK
options into the compiler. Use the explicit native overrides above for native
compiler tuning.

## Generated RTL and headers

Each recipe keeps its own `circt-ingestion` directory and
`.goldengate-circt-complete.json` receipt in `GENERATED_DIR`. The receipt binds:

- Exact FIRRTL and annotation file contents and locations.
- The pinned compiler executable, build configuration, platform, compiler flags
  and publication helper.
- Every published RTL/header/constraint/metadata file and appended blackbox
  resource.

Every Make invocation checks the native build dependencies and receipt.
Matching content reuses the result without rewriting headers or rebuilding a
current driver. Missing, modified or unreceipted outputs trigger compilation;
pre-existing SFC files cannot satisfy this check.

Before a new design compilation the successful receipt is removed and the
ingestion directory is recreated. The compiler must succeed and produce all
required collateral before publication. Input changes during compilation and
missing outputs fail the build. Files are installed atomically and the complete
receipt is written last under a recipe-specific lock. A disabled AutoILA rebuild
removes that recipe's previous generated IP scripts.

## Validation and timing

`test/Support/FireSimCompilerSelectionTest.py` exercises the production compiler
and FPGA delivery Make rules with controlled compiler/build fixtures. It checks
one design invocation across `replace-rtl` then `driver`, invalidation, stale SFC
files, failure, output timestamps and concurrent recipes/compiler rebuilds.
`test/Support/FireSimBootstrapOverlapTest.py` checks the actual Firechip build
fragment, overlap, joins, environment isolation and process-group cleanup.

Production reuse and overlap reduce real work and latency. A warm shared build
does not establish a cold pre-Vivado speedup. Measurements of a cold run must
include native bootstrap, frontend and driver work with matched inputs and
cache/source policies; a different ordinary build root provides isolation.

Optimize the normal production path. Do not add evaluation-specific shortcuts,
skip required compiler stages, or force production rebuilds to imitate a cold
measurement. First-use comparisons start each configuration independently,
without compiler binaries, generated designs, driver outputs or Scala build
products inherited from another configuration. Reusing outputs produced earlier
within that same build (for example, `replace-rtl` followed by `driver`) is normal
production behavior. Report cold and incremental measurements separately, and
include all required work in the advertised end-to-end interval. Compiler-only
measurements exclude executable bootstrap and elaboration and must use identical
frozen FIRRTL/annotation inputs, matched resources and fresh compiler processes.

Normal helper diagnostics identify native configure/build, actual design
compilation, frontend/bootstrap overlap, and receipt reuse separately. They
print the nested argument vectors and supervised wall durations. The native
build check can run on reuse without executing the design compiler.

External build-timing wrappers must trace these nested children and recognize
the `goldengate-circt-<SHA-256>` executable basename. Counting the whole Python
helper as a compiler invocation would include bootstrap, locks, validation and
publication; its duration is not a compiler-only measurement. Overlapped phase
durations also must not be summed as end-to-end elapsed time. The separate
verification checkout's `FIRESIM_BUILD_TIMING` implementation is not included in
this repository; this helper does not invent a replacement schema for it.
