# FAME5 register storage primitive

`threadFAME5Registers` lowers each scalar register to a `threadCount`-entry
memory with combinational read and one-cycle write latency. Both addresses use
the supplied active-thread input. The write clock remains the original target
clock; next-state feedback implements holds, and a synchronous reset selects the
initial value with priority over the ordinary update. Reset therefore modifies
only the selected thread. A non-power-of-two thread count disables writes for
out-of-range index encodings.

The reference is `MuxingMultiThreader.onStmt` in
`midas/src/main/scala/midas/passes/fame/MuxingMultithreader.scala`. The Scala test
invokes that existing implementation on a lowered resettable register fixture,
then executes the emitted memory connect expressions. The native test similarly
executes its emitted FIRRTL expressions and checks every edge against independent
replicated registers. Neither test changes an archived SFC golden.

The focused comparison on 2026-10-23 executed 4,096 edges each for two, three and
four threads, varying selected thread, reset, write enable and data. All selected
read/write addresses and every thread's state matched. State-stream checksums:

| Threads | SFC and CIRCT checksum |
| --- | --- |
| 2 | 15927411740211530219 |
| 3 | 15723135945319982935 |
| 4 | 14858674244570049459 |

The native test also verifies FIRRTL syntax/typing and seven atomic rejection
cases: wrong index width, asynchronous reset, missing connect, duplicate connect,
attached annotation, local raw annotation reference and hierarchical raw
annotation reference. A second invocation rejects already-threaded modules.
Focused logs and generated SFC fixtures are in
`/scratch/jfx/fame5-threaded-state-20261023/`.

Both SFC and CIRCT exported Verilog also built with Verilator 5.022 and ran the
same `FAME5ThreadedStateRTL.cpp` scoreboard through public module ports. After
resetting every slot, it checks the selected value before each edge and all
slots after every edge. The 4,096-edge RTL checksums matched in all three cases:
`74352443707064666` (two threads), `2803239709251869998` (three threads) and
`4667365622890541513` (four threads). Uninitialized storage is not compared.

Run the native check with:

```sh
cmake --build /scratch/jfx/goldengate-circt-f2-build --target goldengate-fame5-threaded-state-test -j2
ctest --test-dir /scratch/jfx/goldengate-circt-f2-build -R '^goldengate-fame5-threaded-state$' --output-on-failure
/scratch/jfx/goldengate-circt-f2-build/goldengate-circt \
  sims/firesim/sim/goldengate-circt/test/Transforms/FAME5ThreadedState.fir \
  --annotation-file sims/firesim/sim/goldengate-circt/test/Transforms/FAME5ThreadedState.anno.json \
  --output-dir /scratch/jfx/fame5-threaded-state-20261023/native4 \
  --thread-fame5-registers Model 4 thread
```

To reproduce the SFC arm, compile `FAME5ThreadedStateOracle.scala` with Scala
2.13.10 using `midas/target/scala-2.13/classes` plus the classpath in
`midas/target/streams/compile/dependencyClasspath/_global/streams/export`.
Run `midas.passes.fame.FAME5ThreadedStateOracle <output-directory>` and compare
its `ORACLE` lines to the native test's `ORACLE` lines.

This is a register-storage primitive and a focused compiler transformation.
Combinational-read target-memory banking is covered separately in
`FAME5ThreadedMemory.md`. Production FAME5 still needs repeated-model grouping,
hierarchy specialization, synchronous-read target-memory response buffering,
channel mux/demux and a coordinated scheduler. Annotated/symbolic register
renames and aggregate or asynchronous state are unsupported and fail closed.
The bounded fixtures do not establish a threaded Linux representative, separate
Jasper LEC/SEC proofs, F2 behavior, or any feature-acceptance receipt.
