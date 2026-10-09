# Anonymous assertions after host assembly inlining

Iteration 63 fixes the Verilator driver compilation failure reported by
`deploy/logs/2026-10-09--04-00-22-builddriver-BGWZ7ADWAP65HJLU.log`.
The full manager log and verification stdout/stderr were read before changing
the compiler. The emitted RTL had no NUL bytes. Its `assert___0` label became
`"assert_\000"s` in Verilator's VPI scope constructor, which requires `const char *`.
The incomplete `__0` escape can corrupt the decoded name differently between
runs; a small isolated reproduction produced `"assert_\222"` instead.

The native inliner prefixes anonymous operations' empty names with instance
paths. Removing repeated `sim_` prefixes must preserve an empty name instead
of passing it to `Namespace::newName`, which invents `_0`. The backend then
emits an unnamed assertion. Named declarations and assertions still receive
collision handling. The source archive, inner symbols, and assertion operands
are preserved.

`HostHierarchyTest.cpp` checks two anonymous assertions and one named assertion
colliding with an existing top declaration at wrapper depths 3 and 98. It
checks names, assertion count, predicate operation, enable constant, and clock
type alongside the existing hierarchy, XDC, symbol, and atomicity cases. Linking
the final test against the preceding compiler revision fails with
`inlined anonymous assertion acquired label: _0`; the fixed compiler passes.

Evidence is under the mutable U250 generated-source directory's
`iteration63-anonymous-assertions/`. Re-emitting the harness's actual
`circt-ingestion/post-xilinx-host-specialization.mlir` removes 97 wrappers.
Comparison to its prior emitted RTL finds exactly two removed assertion labels
after ignoring source-location comments; every other RTL token is unchanged.
All module bodies likewise differ only in those two anonymous names. The
serialized archive is observed at different XDC preparation phases in these
two dumps, so it is not claimed byte-identical.

The exact immutable SFC reference is
`deploy/results-build/2026-10-01--04-55-23-circt_u250_firesim_rocket_singlecore/cl_xilinx_alveo_u250-firesim-FireSim-FireSimRocketConfig-BaseXilinxAlveoU250Config.sfc-golden-2026-10-01/design/FireSim-generated.sv`.
Its LoadMemWidget checks on `crFile_io_mcr_read_4_ready` and
`crFile_io_mcr_write_8_valid` match the candidate's inlined checks: negated
predicates, positive clock edge, reset-disabled enable, and diagnostic messages.
Both emitted XDC files remain unchanged and match the same reference directory
after the established `/target/FireSim_/` to `/target_FAMETop/FireSim/`
hierarchy substitution. `comparison.json` records these contracts.

The host-hierarchy, XDC-emission, and metasim-interface-header CTests pass.
Verilator generation with `--vpi --assert` and a C++ syntax check of the small
native fixture's scope constructor pass. These focused checks do not run the
FireSim manager or a simulation. The harness still owns the fresh driver,
metasim workloads, and subsequent FPGA gates. After this regression is cleared,
the next compiler feature remains selected SRAM integration with Print hosts.
