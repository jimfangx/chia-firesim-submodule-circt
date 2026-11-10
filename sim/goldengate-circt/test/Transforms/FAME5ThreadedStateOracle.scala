// See LICENSE for license details.
// Execute the actual SFC register-to-memory transform. This fixture is not a
// reimplementation of the lowering: transitions below come from onStmt output.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import scala.collection.mutable

object FAME5ThreadedStateOracle extends App {
  for (count <- Seq(2, 3, 4)) {
    val input = s"""circuit Model :
      |  module Model :
      |    input clock : Clock
      |    input thread : UInt<${if (count == 2) 1 else 2}>
      |    input reset : UInt<1>
      |    input enable : UInt<1>
      |    input data : UInt<8>
      |    output result : UInt<8>
      |    reg state : UInt<8>, clock with :
      |      reset => (reset, UInt<8>(7))
      |    state <= mux(enable, data, state)
      |    result <= state
      |""".stripMargin
    val circuit = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil).circuit
    val original = circuit.modules.head.asInstanceOf[Module]
    val resets = mutable.ArrayBuffer[Statement]()
    val index = WRef("thread", UIntType(IntWidth(if (count == 2) 1 else 2)), PortKind, SourceFlow)
    val transformed = MuxingMultiThreader.onStmt(resets, BigInt(count), index)(original.body)
    val body = Block(Seq(transformed) ++ resets)
    val nodes = mutable.Map[String, Expression]()
    val memories = mutable.ArrayBuffer[DefMemory]()
    def collect(stmt: Statement): Unit = {
      stmt match {
        case DefNode(_, name, value) => nodes(name) = value
        case mem: DefMemory => memories += mem
        case _ => ()
      }
      stmt.foreachStmt(collect)
    }
    collect(body)
    require(memories.size == 1)
    val mem = memories.head
    require(mem.depth == count && mem.readLatency == 0 && mem.writeLatency == 1)
    require(mem.readers == Seq("read") && mem.writers == Seq("write"))
    def evaluate(e: Expression, values: Map[String, BigInt]): BigInt = e match {
      case UIntLiteral(value, _) => value
      case ref: WRef => values.getOrElse(ref.name, evaluate(nodes(ref.name), values))
      case ref: WSubField => values(ref.serialize)
      case Mux(c, a, b, _) => evaluate(if (evaluate(c, values) != 0) a else b, values)
      case other => throw new Exception(s"unsupported oracle expression ${other.serialize}")
    }
    def connections(stmt: Statement, values: Map[String, BigInt], result: mutable.Map[String, Expression]): Unit = stmt match {
      case Connect(_, lhs, rhs) => result(lhs.serialize) = rhs
      case Conditionally(_, pred, cons, alt) => connections(if (evaluate(pred, values) != 0) cons else alt, values, result)
      case other => other.foreachStmt(connections(_, values, result))
    }
    val state = Array.tabulate(count)(slot => BigInt(31 + slot))
    var checksum = BigInt(0)
    for (cycle <- 0 until 4096) {
      val thread = (cycle * 7 + cycle / 13) % count
      val values = Map("thread" -> BigInt(thread), "reset" -> BigInt(if (cycle % 37 == 0) 1 else 0),
        "enable" -> BigInt(if ((cycle + thread) % 5 != 0) 1 else 0),
        "data" -> BigInt((cycle * 17 + thread * 29) & 255), "state.read.data" -> state(thread))
      val conns = mutable.Map[String, Expression]()
      connections(body, values, conns)
      require(evaluate(conns("state.read.addr"), values) == thread)
      require(evaluate(conns("state.write.addr"), values) == thread)
      require(evaluate(conns("state.write.mask"), values) == 1)
      if (evaluate(conns("state.write.en"), values) != 0)
        state(thread) = evaluate(conns("state.write.data"), values)
      for (slot <- 0 until count)
        checksum = (checksum * 131 + state(slot) + slot) & ((BigInt(1) << 64) - 1)
    }
    println(s"ORACLE $count $checksum")
    if (args.nonEmpty) {
      val source = new java.io.PrintWriter(new java.io.File(args(0), s"input-$count.sfc.fir"))
      try source.write(input) finally source.close()
      val out = new java.io.PrintWriter(new java.io.File(args(0), s"threaded-state-$count.sfc.fir"))
      try out.write(original.copy(body = body).serialize) finally out.close()
      val emitted = new VerilogCompiler().compileAndEmit(
        CircuitState(circuit.copy(modules = Seq(original.copy(body = body))), HighForm), Nil)
      val rtl = new java.io.PrintWriter(new java.io.File(args(0), s"threaded-state-$count.sfc.v"))
      try rtl.write(emitted.getEmittedCircuit.value) finally rtl.close()
    }
  }
}
