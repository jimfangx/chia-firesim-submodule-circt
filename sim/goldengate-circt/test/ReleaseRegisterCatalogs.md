# Release register catalog lifetime

Iteration 10 fixes undefined behavior in `SingleClockBridge.cpp`,
`ResetPulseBridge.cpp` and `SimulationMaster.cpp`. Their register-name loops
passed a temporary initializer-list-backed `ArrayRef<StringRef>` into
`llvm::enumerate`. Under C++17, the backing array expired before loop execution.
All three now enumerate static constant arrays. Ordered register names, offsets,
permissions and hardware are preserved. The three transform tests now assert
the exact ordered names alongside their existing offset/permission checks.

CHIA iteration 9 built the native Release compiler successfully, then all four
selected target compilations exited with SIGSEGV. Their final complete boundaries
were SRAM simulator hardware for Rocket/MegaBoomV3 and active FAME wrappers for
Shuttle/Saturn Shuttle, immediately before common ClockBridge construction.
The small existing Release `goldengate-single-clock-bridge-test` reproduced
SIGSEGV in string hashing through `StringAttr::get` and `addClockBridge`.
The debugger trace is `iteration10-clock-before-gdb.log` in the evidence directory
below. No full target replay was launched by the implementation agent.

The Scala contracts remain authoritative: `ClockBridge.scala` constructs hCycle
and tCycle wide counters in order; `Widget.scala` emits two read-only words then
one write-only latch for each. `ResetPulseBridge.scala` emits pulseLength and
doneInit; `Master.scala` emits INIT_DONE, PRESENCE_READ and PRESENCE_WRITE.
The latter helpers use default ReadWrite bindings even for helpers named RO/WO.
The existing component tests cover their scheduling/reset/write behavior and
malformed-boundary rejection, in addition to the catalog assertions.

With the existing Release build configured with `BUILD_TESTING=ON`, run:

```sh
.conda-env/bin/cmake --build /scratch/jfx/goldengate-circt-f2-release-build \
  --parallel 8 --target goldengate-circt goldengate-single-clock-bridge-test \
  goldengate-reset-pulse-bridge-test goldengate-simulation-master-test \
  goldengate-clock-bridge-header-test goldengate-reset-pulse-header-test \
  goldengate-simulation-master-header-test
.conda-env/bin/ctest --test-dir /scratch/jfx/goldengate-circt-f2-release-build \
  --output-on-failure \
  -R '^goldengate-(single-clock-bridge|reset-pulse-bridge|simulation-master|clock-bridge-header|reset-pulse-header|simulation-master-header|f2-compiler-config|debug-snapshot-policy)$'
```

The compiler and all eight focused tests pass with `-O3 -DNDEBUG`. The name
assertions use independent expected arrays. A read-only audit compares those
asserted register names and local offsets, plus C++ struct offsetof indices,
against all four exact immutable SFC headers: twelve register tables pass.
Absolute target-specific addresses are retained in the report; only each bank's
base is subtracted for the local offset check. Headers do not encode permissions;
the Scala helpers and component behavior checks cover that separate contract.
This is a scoped metadata comparison, not full constructor or whole-target ABI
parity. All four immutable receipt/manifest/full-RTL identities match iteration 9;
the complete input/resource/artifact inventories also revalidate.

Evidence is under `/scratch/jfx/goldengate-circt-f2-build/`:

- `iteration10-release-build.log`, `iteration10-tests-rebuild.log`,
  `iteration10-header-build.log`
- `iteration10-release-ctest.log`, `iteration10-header-ctest.log`
- `iteration10-register-contract-audit.py` and `.json`
- `iteration10-golden-identities.json`, `iteration10-implementation-result.json`

Next CHIA rebuilds as configured and retries the exact four archived input pairs.
The candidate source inventory invalidates old receipts automatically. Complete
target compilation/semantic comparisons, separate LEC/SEC, F2 runtime and faster
matched compiler timing remain incomplete. No component test duration is a
compiler-speed result.
