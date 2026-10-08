// See LICENSE for license details.
// Independent SFC preparation/FAME/RAM/XDC oracle. Native candidates are only
// parsed; no Scala compiler transform runs on their hardware.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.{DefineAbstractClockGate, WriteXDCFile, XDCOutputAnnotation}
import midas.targetutils.xdc.XDCPathToCircuitAnnotation
import scala.collection.mutable
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMTimingModelsOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String) = AsyncRAMModelFiles.read(new java.io.File(directory, path))
  def write(path: String, body: String) = AsyncRAMModelFiles.write(new java.io.File(directory, path), body)
  for (name <- Seq("fanout", "golden-rocket", "grouped-rocket")) {
    val sourceName = if (name == "grouped-rocket") "golden-rocket" else name
    val text = read(s"oracle/$sourceName.channels-input.fir")
    // Keep the existing readwrite probe immutable; async RAM supports distinct
    // read and write commands. Four promoted uses still share one definition.
    val inputText = if (name == "fanout") text
      .replaceAll("(?m)^\\s*readwriter => rw\\n", "")
      .replaceAll("(?m)^\\s*ram\\.rw[^\\n]*\\n", "") else text
    val originalAnnos = JsonProtocol.deserialize(read(s"oracle/$sourceName.channels-input.json"))
    // Use actual Rocket leaves from the immutable input. Reverse their order
    // so the oracle tests ordered multiport input and output payloads.
    val annos = if (name != "grouped-rocket") originalAnnos else {
      val external = originalAnnos.collect {
        case c: FAMEChannelConnectionAnnotation if c.globalName.startsWith("external_io_interrupts_") ||
          Set("external_io_imem_req_bits_pc", "external_io_imem_req_bits_speculative")(c.globalName) => c
      }
      val inputs = external.flatMap(_.sinks.toSeq.flatten).reverse
      val outputs = external.flatMap(_.sources.toSeq.flatten).reverse
      originalAnnos.filterNot(external.contains) ++ Seq(
        FAMEChannelConnectionAnnotation.sink("io_interrupts", PipeChannel(0), external.head.clock, inputs),
        FAMEChannelConnectionAnnotation.source("io_imem_req_bits", PipeChannel(0), external.head.clock, outputs))
    }
    val input = new ResolveAndCheck().runTransform(CircuitState(Parser.parse(inputText), LowForm, annos))
    val labeled = new LabelSRAMModels().runTransform(input)
    val promoted = new ExtractModel().runTransform(labeled)
    val lowered = new LowFirrtlCompiler().compile(promoted, Nil)
    val passthrough = new ResolveAndCheck().runTransform(PromotePassthroughConnections.runTransform(lowered))
    val defaults = new FAMEDefaults().runTransform(passthrough)
    val clocked = FindDefaultClocks.runTransform(defaults)
    val channels = new InferModelPorts().runTransform(new ChannelExcision().runTransform(clocked))
    write(s"$name.input.fir", inputText)
    write(s"$name.input.json", JsonProtocol.serialize(input.annotations :+
      XDCPathToCircuitAnnotation(Some("pre/link"), Some("post/link"))))
    write(s"oracle/$name.prepared.sfc.fir", channels.circuit.serialize)
    val transformed = AddRemainingFanoutAnnotations.runTransform(new FAMETransform().runTransform(
      new ResolveAndCheck().runTransform(channels)))
    val fame = transformed.copy(circuit = transformed.circuit.copy(
      modules = transformed.circuit.modules :+ DefineAbstractClockGate.blackbox))
    val ram = new EmitAndWrapRAMModels().runTransform(fame)
    write(s"$name.expected.fir", ram.circuit.serialize)
    write(s"$name.expected.json", JsonProtocol.serialize(ram.annotations))
    // SFC's pre-SimWrapper circuit is embedded under the native transport
    // container. The prefix supplies only that outer boundary; WriteXDCFile
    // independently expands every actual hub and gate instance below it.
    val xdc = WriteXDCFile.runTransform(ram.copy(annotations = ram.annotations :+
      XDCPathToCircuitAnnotation(Some("pre/link/target_FAMETop"), Some("post/link/target_FAMETop"))))
    xdc.annotations.collect { case a: XDCOutputAnnotation =>
      write(s"$name.expected${a.suffix.get}", a.fileBody)
    }
    println(s"SFC $name: independent preparation, FAME, RAM replacement and WriteXDCFile")
  }
}

object SRAMTimingModelsCompare extends App {
  val directory = new java.io.File(args(0))
  def read(path: String) = AsyncRAMModelFiles.read(new java.io.File(directory, path))
  def circuit(path: String) = Parser.parse(read(path).replaceAll("(?m)^(\\s*)public module ", "$1module "))
  def statements(s: Statement): Seq[Statement] = {
    val result = mutable.ArrayBuffer[Statement]()
    def visit(s: Statement): Unit = { result += s; s.foreachStmt(visit) }
    visit(s); result.toSeq
  }
  def multiset[A](values: Seq[A]) = values.groupMapReduce(identity)(_ => 1)(_ + _)
  for ((name, memory, hub, count) <- Seq(("fanout", "ram", "Top", 4), ("golden-rocket", "rf", "Rocket", 1), ("grouped-rocket", "rf", "Rocket", 1))) {
    val native = circuit(s"$name-native/post-sram-models.fir")
    val expected = circuit(s"$name.expected.fir")
    def module(c: Circuit, n: String) = c.modules.find(_.name == n).get.asInstanceOf[Module]
    def abi(m: Module) = m.ports.map(p => p.name -> (p.direction, p.tpe.serialize.replace(" ", ""))).toMap
    require(abi(module(native, hub)) == abi(module(expected, hub)), s"$name hub ABI changed")
    val n = module(native, memory); val s = module(expected, memory)
    require(abi(n) == abi(s), s"$name SRAM adapter ABI differs")
    def equations(m: Module) = {
      val stmts = statements(m.body)
      val nodes = stmts.collect { case DefNode(_, name, value) => name -> value }.toMap
      def canonical(e: Expression): String = e match {
        case UIntLiteral(value, _) => value.toString
        case r: WRef if nodes.contains(r.name) => canonical(nodes(r.name))
        case DoPrim(PrimOps.And, Seq(a, b), _, _) =>
          def terms(e: Expression): Seq[String] = e match {
            case r: WRef if nodes.contains(r.name) => terms(nodes(r.name))
            case DoPrim(PrimOps.And, Seq(x, y), _, _) => terms(x) ++ terms(y)
            case other => Seq(canonical(other))
          }
          val vs = (terms(a) ++ terms(b)).distinct.sorted.filterNot(_ == "1")
          if (vs.isEmpty) "1" else if (vs.size == 1) vs.head else s"and(${vs.mkString(",")})"
        case DoPrim(PrimOps.AsUInt, Seq(v), _, _) => canonical(v)
        case DoPrim(PrimOps.Or, Seq(a, b), _, _) =>
          def terms(e: Expression): Seq[String] = e match {
            case r: WRef if nodes.contains(r.name) => terms(nodes(r.name))
            case DoPrim(PrimOps.Or, Seq(x, y), _, _) => terms(x) ++ terms(y)
            case other => Seq(canonical(other))
          }
          val vs = (terms(a) ++ terms(b)).distinct.sorted
          if (vs.contains("1")) "1" else {
            val terms = vs.filterNot(_ == "0")
            if (terms.isEmpty) "0" else if (terms.size == 1) terms.head else s"or(${terms.mkString(",")})"
          }
        case DoPrim(PrimOps.Not, Seq(v), _, _) => s"not(${canonical(v)})"
        case Mux(c, t, f, _) => s"mux(${canonical(c)},${canonical(t)},${canonical(f)})"
        case other => other.serialize
      }
      stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> canonical(rhs) }.toMap
    }
    require(equations(n) == equations(s), s"$name indexed adapter equations differ")
    val nativeHub = module(native, hub); val expectedHub = module(expected, hub)
    val hubEquations = equations(nativeHub)
    val hubPorts = expectedHub.ports.map(_.name).toSet
    def fameRegister(name: String) = name.endsWith("_enabled") || name.matches(".*_fired_[0-9]+")
    val hubRegisters = statements(expectedHub.body).collect {
      case r: DefRegister if fameRegister(r.name) => r.name
    }.toSet
    val referenceHubEquations = equations(expectedHub).filter { case (key, _) =>
      hubRegisters(key) || key == "targetCycleFinishing" || key.endsWith("_buffer.I") ||
        key.endsWith("_buffer.CE") || (hubPorts(key.takeWhile(_ != '.')) &&
          (key.endsWith(".ready") || key.endsWith(".valid")))
    }
    val differing = referenceHubEquations.toSeq.filter { case (key, value) => !hubEquations.get(key).contains(value) }
    require(differing.isEmpty, s"$name hub equations differ: ${differing.take(3)}")
    def registers(m: Module) = statements(m.body).collect {
      case r: DefRegister if fameRegister(r.name) => r.name -> (r.tpe.serialize, r.clock.serialize, r.reset.serialize, r.init.serialize)
    }.toMap
    require(registers(nativeHub) == registers(expectedHub), s"$name hub register contracts differ")
    require(!statements(n.body).exists(s => s.isInstanceOf[DefMemory] || s.isInstanceOf[DefRegister]),
      s"$name original SRAM/FAME state survived replacement")
    val ramInstances = native.modules.collect { case m: Module => statements(m.body).collect {
      case i: WDefInstance if i.module == memory => i
    }}.flatten
    require(ramInstances.size == count, s"$name promoted SRAM multiplicity differs")
    val nativeInner = module(native, "FAMETop")
    val expectedInner = module(expected, expected.main)
    def hubInstance(m: Module) = statements(m.body).collectFirst {
      case i: WDefInstance if i.module == hub => i.name
    }.get
    val oldHub = hubInstance(expectedInner); val newHub = hubInstance(nativeInner)
    for (suffix <- Seq(".implementation.xdc", ".synthesis.xdc")) {
      val reference = read(s"$name.expected$suffix").replace(s"/$oldHub/", s"/$newHub/")
      require(read(s"$name-native/post-sram-models$suffix") == reference, s"$name $suffix differs")
    }
    val nAnnos = JsonProtocol.deserialize(read(s"$name-native/post-sram-models-all.json"))
      .filterNot(_.isInstanceOf[XDCOutputAnnotation])
    val sAnnos = JsonProtocol.deserialize(read(s"$name.expected.json"))
      .filterNot(_.isInstanceOf[midas.InternalXDCAnnotation])
    // Hub-instance uniquing is permitted; transfer only that hierarchical
    // target component and the matching global channel prefix.
    def canonical(value: JValue): JValue = value match {
      case JObject(fields) => JObject(fields.sortBy(_._1).map { case (k, v) => k -> canonical(v) })
      case JArray(values) => JArray(values.map(canonical))
      case other => other
    }
    def normalize(a: Annotation, candidate: Boolean): String = {
      val text = if (candidate) {
        val renamed = JsonProtocol.serialize(Seq(a)).replace(s"~${native.main}|", s"~${expected.main}|")
          .replace(s"|${native.main}>", s"|${expected.main}>")
        a match {
          // Native transport converts the scalar Clock payload to its single
          // Boolean vector lane; the pre-wrapper SFC endpoint is scalar.
          case c: FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[TargetClockChannel] =>
            renamed.replace("_clock_sink.bits[0]", "_clock_sink.bits")
          case _ => renamed
        }
      } else JsonProtocol.serialize(Seq(a)).replace(s"${oldHub}_", s"${newHub}_")
      compact(render(canonical(parse(text))))
    }
    require(multiset(nAnnos.map(a => normalize(a, true))) == multiset(sAnnos.map(a => normalize(a, false))),
      s"$name retained annotation multiset differs")
    println(s"PASS $name: ${equations(s).size} adapter equations, hub/adapter ABIs, $count instances, ${nAnnos.size} annotations, ${referenceHubEquations.size} hub control equations and both XDC files")
  }
}
