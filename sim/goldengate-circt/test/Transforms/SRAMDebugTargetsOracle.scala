// See LICENSE for license details.
// Independent SFC preparation and FAME; native hardware is only parsed.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.InternalFirrtlFpgaDebugAnnotation
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMDebugTargetsOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  def write(path: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, path))
    try out.write(text) finally out.close()
  }
  // Recheck the recorded handoff against the immutable compiler fixture;
  // this validates actual port and memory contracts before testing debug
  // selections added only to the mutable comparison inputs.
  val goldenSource = scala.io.Source.fromFile(args(1))
  val golden = try Parser.parse(goldenSource.mkString) finally goldenSource.close()
  val goldenLow = firrtl.passes.RemoveValidIf.runTransform(
    new LowFirrtlCompiler().compile(CircuitState(golden, HighForm, Nil), Nil)).circuit
  val goldenRocket = goldenLow.modules.find(_.name == "Rocket").get.asInstanceOf[Module]
  def abi(module: DefModule) = module.ports.map(p => (p.name, p.direction, p.tpe.serialize)).toSet
  def memoryContracts(module: Module): Map[String, String] = {
    val memories = scala.collection.mutable.Map[String, String]()
    def visit(statement: Statement): Unit = {
      statement match {
        case memory: DefMemory => memories(memory.name) = memory.copy(info = NoInfo).serialize
        case _ =>
      }
      statement.foreachStmt(visit)
    }
    visit(module.body); memories.toMap
  }
  def probes(text: String): Seq[String] = parse(text).asInstanceOf[JArray].arr.collect {
    case annotation if (annotation \ "class") == JString("midas.InternalFirrtlFpgaDebugAnnotation") =>
      (annotation \ "target").asInstanceOf[JString].s
  }
  def probeIdentity(circuit: Circuit, probe: String): String = {
    val parts = probe.split('.').toSeq
    if (parts(1) != circuit.main) return probe
    val top = circuit.modules.find(_.name == circuit.main).get.asInstanceOf[Module]
    val instances = scala.collection.mutable.Map[String, String]()
    val bindings = scala.collection.mutable.Map[String, String]()
    def visit(statement: Statement): Unit = {
      statement match {
        case instance: WDefInstance => instances(instance.name) = instance.module
        case Connect(_, WSubField(WRef(instance, _, _, _), port, _, _), WRef(topPort, _, _, _)) =>
          bindings(topPort) = s"$instance.$port"
        case Connect(_, WRef(topPort, _, _, _), WSubField(WRef(instance, _, _, _), port, _, _)) =>
          bindings(topPort) = s"$instance.$port"
        case _ =>
      }
      statement.foreachStmt(visit)
    }
    visit(top.body)
    val binding = bindings(parts(2)).split('.')
    s"${parts.head}.wrapper:${instances(binding(0))}.${binding(1)}" +
      parts.drop(3).map("." + _).mkString
  }
  for (name <- Seq("golden-rocket", "grouped-rocket")) {
    val recorded = Parser.parse(read(s"$name.input.fir"))
      .modules.find(_.name == "Rocket").get.asInstanceOf[Module]
    require(abi(recorded).filterNot(_._1 == "external_clock") == abi(goldenRocket),
      "recorded Rocket ports differ from immutable SFC handoff")
    require(memoryContracts(recorded) == memoryContracts(goldenRocket),
      "recorded Rocket memories differ from immutable SFC handoff")
    val input = new ResolveAndCheck().runTransform(CircuitState(
      Parser.parse(read(s"$name.input.fir")), LowForm,
      JsonProtocol.deserialize(read(s"$name.input.json"))))
    val labeled = new LabelSRAMModels().runTransform(input)
    val promoted = new ExtractModel().runTransform(labeled)
    val lowered = new LowFirrtlCompiler().compile(promoted, Nil)
    val passthrough = new ResolveAndCheck().runTransform(
      PromotePassthroughConnections.runTransform(lowered))
    val clocked = FindDefaultClocks.runTransform(new FAMEDefaults().runTransform(passthrough))
    val channels = new InferModelPorts().runTransform(new ChannelExcision().runTransform(clocked))
    val expected = new FAMETransform().runTransform(new ResolveAndCheck().runTransform(channels))
    val expectedJSON = JsonProtocol.serialize(expected.annotations)
    write(s"$name.expected.fir", expected.circuit.serialize)
    write(s"$name.expected.json", expectedJSON)
    val actualText = read(s"$name-native/post-sram-parent-fame.fir")
      .replaceAll("(?m)^(\\s*)public module ", "$1module ")
    val actual = Parser.parse(actualText)
    val actualProbes = probes(read(s"$name-native/post-sram-parent-fame-all.json"))
    val expectedProbes = probes(expectedJSON)
    // LowForm coalesces the repeated input selector in both compilers.
    require(expectedProbes.size == 6 &&
      actualProbes.map(probeIdentity(actual, _)).sorted ==
        expectedProbes.map(probeIdentity(expected.circuit, _)).sorted,
      s"$name debug target transfer differs: expected=$expectedProbes actual=$actualProbes")
    // Resolve every selection against the candidate ABI, including ordered
    // multiport bits fields. An annotation pointing at an erased port fails.
    for (probe <- actualProbes) {
      val parts = probe.split('.').toSeq
      val module = actual.modules.find(_.name == parts(1)).get
      val port = module.ports.find(_.name == parts(2)).get
      val leaf = parts.drop(3).foldLeft(port.tpe) { (tpe, field) =>
        tpe.asInstanceOf[BundleType].fields.find(_.name == field).get.tpe
      }
      require(leaf.isInstanceOf[UIntType] || leaf.isInstanceOf[SIntType], s"noninteger probe $probe")
    }
    require(expected.annotations.count(_.isInstanceOf[InternalFirrtlFpgaDebugAnnotation]) == 6)
    println(s"PASS $name: six SFC debug targets, duplicate coalescing and resolved payload types")
  }
}
