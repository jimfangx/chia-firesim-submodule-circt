// See LICENSE for license details.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import scala.collection.mutable

// Invoke unchanged SFC MuxingMultiThreader and ImplementThreadedMems on the
// exact saved input subsequently replayed through the CIRCT compiler.
object FAME5ThreadedMemoryOracle extends App {
  val source = scala.io.Source.fromFile(args(0))
  val input = try source.mkString finally source.close()
  val count = args(1).toInt
  val directory = new java.io.File(args(2))
  directory.mkdirs()
  def write(name: String, value: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, name))
    try out.write(value) finally out.close()
  }
  val lowered = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil).circuit
  val original = lowered.modules.head.asInstanceOf[Module]
  val prototypes = mutable.ArrayBuffer[DefMemory]()
  def originalMemories(stmt: Statement): Unit = {
    stmt match { case mem: DefMemory => prototypes += mem; case _ => () }
    stmt.foreachStmt(originalMemories)
  }
  originalMemories(original.body)
  require(prototypes.nonEmpty && prototypes.forall(m => m.depth == 3 && m.writeLatency == 1))
  val readLatency = prototypes.head.readLatency
  require(prototypes.forall(_.readLatency == readLatency))
  val resets = mutable.ArrayBuffer[Statement]()
  val body = MuxingMultiThreader.onStmt(resets, count, WRef("threadIdx"))(original.body)
  require(resets.isEmpty)
  val threaded = ImplementThreadedMems(lowered.copy(modules = Seq(original.copy(body = body))))
  val memories = mutable.ArrayBuffer[DefMemory]()
  def collect(stmt: Statement): Unit = {
    stmt match { case mem: DefMemory => memories += mem; case _ => () }
    stmt.foreachStmt(collect)
  }
  threaded.modules.collect { case module: Module => module }.foreach(m => collect(m.body))
  val targetBanks = memories.filter(_.depth == count * 3)
  val responseBanks = memories.filter(_.depth == count)
  // ImplementThreadedMems shares identical helper-module definitions, including
  // the independently instantiated lo/hi banks produced by LowerTypes.
  val implementations = prototypes.map(_.copy(name = "")).distinct
  require(targetBanks.size == implementations.size && targetBanks.forall(m => m.readLatency == readLatency && m.writeLatency == 1))
  val responseCount = if (readLatency == 0) 0 else implementations.map(m => m.readers.size + m.readwriters.size).sum
  require(responseBanks.size == responseCount && responseBanks.forall(m => m.readLatency == (if (count < 4) 0 else 1) && m.writeLatency == 1))
  require(targetBanks.size + responseBanks.size == memories.size)
  write("threaded.sfc.fir", threaded.serialize)
  val emitted = new VerilogCompiler().compileAndEmit(CircuitState(threaded, HighForm), Nil)
  write("threaded.sfc.v", emitted.getEmittedCircuit.value)
  println(s"SFC $count threads: ${targetBanks.size} target banks, ${responseBanks.size} response banks, target depth ${count * 3}, read-latency $readLatency, write-latency 1")
}
