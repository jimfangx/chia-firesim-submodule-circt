// See LICENSE for license details.
package midas.passes.fame
import firrtl._
import firrtl.annotations.JsonProtocol
import firrtl.ir._
import midas.stage.phases.ConfigParametersAnnotation

// Execute the unchanged complete SFC MultiThreadFAME5Models pass, not a copied
// scheduler. The native compiler imports the very same input/annotation files.
object FAME5GroupedModelsOracle extends App {
  def read(path: String): String = { val in = scala.io.Source.fromFile(path); try in.mkString finally in.close() }
  val directory = new java.io.File(args(2)); directory.mkdirs()
  def write(name: String, value: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, name)); try out.write(value) finally out.close()
  }
  val config = ConfigParametersAnnotation(new midas.SimConfig().toInstance.alterPartial {
    case midas.EnableModelMultiThreading => true
  })
  val annotations = JsonProtocol.deserialize(read(args(1)), true) :+ config
  val high = new HighFirrtlCompiler().compile(CircuitState(Parser.parse(read(args(0))), ChirrtlForm, annotations), Nil)
  val result = MultiThreadFAME5Models.execute(high)
  val top = result.circuit.modules.collectFirst { case m: Module if m.name == result.circuit.main => m }.get
  def flat(stmt: Statement): Seq[Statement] = stmt match { case Block(stmts) => stmts.flatMap(flat); case other => Seq(other) }
  val instances = flat(top.body).collect { case i: WDefInstance => i }
  require(instances.size == 1 && instances.head.module.startsWith("Worker_threaded"))
  val threaded = result.circuit.modules.collectFirst { case m: Module if m.name == instances.head.module => m }.get
  val registers = flat(threaded.body).collect { case r: DefRegister => r }
  val memories = flat(threaded.body).collect { case m: DefMemory => m }
  val state = memories.find(_.name == "count").orElse(memories.find(_.name == "state")).get
  val n = state.depth.toInt
  if (read(args(0)).contains("mem store")) {
    // Complete unchanged grouping also introduces an implementation module for
    // each synchronous target RAM; leave those helpers visible to RTL lowering.
    require(memories.size == 1 && memories.head.readLatency == 0)
    require(registers.size == 1)
    val allMemories = result.circuit.modules.collect { case m: Module => flat(m.body).collect { case d: DefMemory => d } }.flatten
    require(allMemories.exists(m => m.depth == 3 * n && m.readLatency == 1 && m.readUnderWrite == ReadUnderWrite.Old))
    require(allMemories.exists(m => m.depth == n && m.readLatency == (if (n < 4) 0 else 1) && m.name != state.name))
  } else require(registers.size == 1 && memories.size == 1 && memories.head.readLatency == 0 && memories.head.writeLatency == 1)
  require(Seq(2,3,4).contains(n))
  write("threaded.sfc.fir", result.circuit.serialize)
  write("threaded.sfc.json", JsonProtocol.serialize(result.annotations.filterNot(_.isInstanceOf[ConfigParametersAnnotation])))
  val emitted = new VerilogCompiler().compileAndEmit(result.copy(form = HighForm), Nil)
  write("threaded.sfc.v", emitted.getEmittedCircuit.value)
  if (read(args(0)).contains("extmodule AbstractClockGate")) write("AbstractClockGate.v", midas.passes.DefineAbstractClockGate.impl)
  println(s"SFC grouped $n repeated Worker instances into ${instances.head.module}; one register bank, static host-cycle mux/demux")
}
