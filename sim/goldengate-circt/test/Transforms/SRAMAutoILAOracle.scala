// See LICENSE for license details.
// Independent SFC AutoILA on the immutable Rocket handoff. Full selected SRAM
// assembly retains Rocket within the FireSim FAME model; these local ports
// keep their original identities while the selected rf becomes a RAM model.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.InternalFirrtlFpgaDebugAnnotation
import midas.stage.{GoldenGateOutputFileAnnotation, OutputBaseFilenameAnnotation}
import midas.stage.phases.ConfigParametersAnnotation
import org.chipsalliance.cde.config.Parameters
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMAutoILAOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(path)
    try source.mkString finally source.close()
  }
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, name))
    try out.write(text) finally out.close()
  }
  val golden = Parser.parse(read(args(1)))
  val goldenLow = firrtl.passes.RemoveValidIf.runTransform(
    new LowFirrtlCompiler().compile(CircuitState(golden, HighForm, Nil), Nil)).circuit
  def rocket(circuit: Circuit) = circuit.modules.find(_.name == "Rocket").get.asInstanceOf[Module]
  val memories = scala.collection.mutable.ArrayBuffer[DefMemory]()
  def visit(statement: Statement): Unit = {
    statement match { case memory: DefMemory => memories += memory; case _ => }
    statement.foreachStmt(visit)
  }
  visit(rocket(goldenLow).body)
  require(memories.exists(m => m.name == "rf" && m.depth == 31 && m.dataType == UIntType(IntWidth(64))),
    "selected register-file shape is absent from immutable handoff")
  val selectors = Seq("reset", "io_imem_req_bits_pc", "io_imem_resp_bits_data")
    .map(name => InternalFirrtlFpgaDebugAnnotation(
      ComponentName(name, ModuleName("Rocket", CircuitName(goldenLow.main)))))
  // The target handoff predates host assembly. Supply only a host clock input
  // for the unchanged SFC host phase; preserve every target module/body.
  val top = goldenLow.modules.find(_.name == goldenLow.main).get.asInstanceOf[Module]
  val hostClock = Namespace(top).newName("hostClock")
  val hostTop = top.copy(ports = top.ports :+ Port(NoInfo, hostClock, Input, ClockType))
  val ground = CircuitState(goldenLow.copy(modules = goldenLow.modules.map {
    case module if module.name == top.name => hostTop
    case module => module
  }), LowForm, selectors :+ midas.passes.HostClockSource(ModuleTarget(goldenLow.main, top.name).ref(hostClock)))
  write("sfc-pre-autoila.fir", ground.circuit.serialize)
  write("sfc-pre-autoila.json", JsonProtocol.serialize(ground.annotations))
  val selected = ground.annotations.collect { case a: InternalFirrtlFpgaDebugAnnotation => a.target.name }.toSet
  val expected = rocket(ground.circuit).ports.filter(p => selected(p.name))
    .map(p => (p.name, p.tpe.asInstanceOf[GroundType].width.asInstanceOf[IntWidth].width.toInt))
  val candidate = parse(read(new java.io.File(directory, "selected-full/autoila-probes.json").toString))
    .asInstanceOf[JArray].arr
  val actual = candidate.zipWithIndex.map { case (entry, index) =>
    require((entry \ "index") == JInt(index) && (entry \ "is_port") == JBool(true))
    val target = (entry \ "target").asInstanceOf[JString].s.split('.').toSeq
    require(target.size == 3 && target(1) == "Rocket", "probe moved out of selected parent model")
    (target(2), (entry \ "width").asInstanceOf[JInt].num.toInt)
  }
  require(expected.size == 3 && actual == expected,
    s"assembled probe identity/order/width differs from immutable SFC target: $actual versus $expected")
  val parameters = Parameters.empty.alterPartial {
    case midas.EnableAutoILA => true
    case midas.ILADepthKey => 2048
    case midas.ILAProbeTriggersKey => 4
  }
  val ila = midas.passes.AutoILATransform.execute(ground.copy(annotations =
    ground.annotations ++ Seq(ConfigParametersAnnotation(parameters), OutputBaseFilenameAnnotation("FireSim-generated"))))
  write("sfc-post-autoila.fir", ila.circuit.serialize)
  write("sfc-post-autoila.json", JsonProtocol.serialize(
    ila.annotations.filterNot(_.isInstanceOf[ConfigParametersAnnotation])))
  require(!ila.annotations.exists(_.isInstanceOf[InternalFirrtlFpgaDebugAnnotation]))
  val script = ila.annotations.collectFirst {
    case a: GoldenGateOutputFileAnnotation if a.fileSuffix.endsWith(".ipgen.tcl") => a.body
  }.get
  write("sfc-ila.ipgen.tcl", script)
  val nativeScript = read(new java.io.File(directory, "selected-full/FireSim-generated.ila_firesim.ipgen.tcl").toString)
  val setting = "CONFIG\\.[A-Z0-9_]+ \\{[^}]+\\}".r
  require(setting.findAllIn(script).toSeq.sorted == setting.findAllIn(nativeScript).toSeq.sorted,
    "assembled ILA IP parameters differ from SFC")
  println("PASS immutable Rocket handoff: three assembled SRAM parent probes match SFC source identity/order/width and all ILA IP settings")
}
