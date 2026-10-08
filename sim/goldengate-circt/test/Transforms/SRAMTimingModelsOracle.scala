// See LICENSE for license details.
// Independent SFC preparation/FAME/RAM/XDC oracle. Native candidates are only
// parsed; no Scala compiler transform runs on their hardware.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.{DefineAbstractClockGate, WriteXDCFile, XDCOutputAnnotation}
import midas.targetutils.xdc.XDCPathToCircuitAnnotation
import midas.core.{SimWrapper, SimWrapperConfig}
import org.chipsalliance.cde.config.Parameters
import scala.collection.mutable
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMTimingModelsOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String) = AsyncRAMModelFiles.read(new java.io.File(directory, path))
  def write(path: String, body: String) = AsyncRAMModelFiles.write(new java.io.File(directory, path), body)
  for (name <- Seq("fanout", "golden-rocket", "grouped-rocket", "ready-valid-rocket")) {
    val sourceName = if (name.endsWith("-rocket")) "golden-rocket" else name
    val text = read(s"oracle/$sourceName.channels-input.fir")
    // Keep the existing readwrite probe immutable; async RAM supports distinct
    // read and write commands. Four promoted uses still share one definition.
    val inputText = if (name == "fanout") text
      .replaceAll("(?m)^\\s*readwriter => rw\\n", "")
      .replaceAll("(?m)^\\s*ram\\.rw[^\\n]*\\n", "") else text
    val originalAnnos = JsonProtocol.deserialize(read(s"oracle/$sourceName.channels-input.json"))
    // Use actual Rocket leaves from the immutable input. Reverse their order
    // so the oracle tests ordered multiport input and output payloads.
    val annos = if (name == "ready-valid-rocket") {
      // Use real Rocket request/response leaves in both bridge orientations.
      // The separate reverse token transports target-ready; target-valid is
      // one field of the forward FAME payload, distinct from host-valid.
      val prefixes = Seq("io_dmem_req", "io_imem_resp")
      val external = originalAnnos.collect {
        case c: FAMEChannelConnectionAnnotation if prefixes.exists(p => c.globalName.startsWith("external_" + p + "_")) => c
      }
      val pairs = prefixes.flatMap { prefix =>
        val selected = external.filter(_.globalName.startsWith("external_" + prefix + "_"))
        val readyChannel = selected.find(_.globalName == "external_" + prefix + "_ready").get
        val forward = selected.filterNot(_ == readyChannel)
        val source = forward.head.sources.nonEmpty
        val leaves = forward.flatMap(c => (if (source) c.sources else c.sinks).toSeq.flatten)
        val valid = leaves.find(_.ref == prefix + "_valid").get
        val ready = (if (source) readyChannel.sinks else readyChannel.sources).get.head
        val info = if (source) DecoupledForwardChannel.source(valid, ready)
                   else DecoupledForwardChannel.sink(valid, ready)
        Seq(FAMEChannelConnectionAnnotation(prefix + "_fwd", info, forward.head.clock,
              if (source) Some(leaves) else None, if (source) None else Some(leaves)),
            readyChannel.copy(globalName = prefix + "_rev", channelInfo = DecoupledReverseChannel))
      }
      originalAnnos.filterNot(external.contains) ++ pairs
    } else if (name != "grouped-rocket") originalAnnos else {
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
    if (name == "ready-valid-rocket") {
      implicit val parameters: Parameters = Parameters.empty
      val top = ram.circuit.modules.find(_.name == ram.circuit.main).get
      val types = top.ports.map(p => ModuleTarget(ram.circuit.main, top.name).ref(p.name) -> p).toMap
      val chirrtl = (new chisel3.stage.ChiselStage).emitChirrtl(
        new SimWrapper(SimWrapperConfig(ram.annotations, types)),
        Array("--target-dir", new java.io.File(directory, "oracle/ready-valid-wrapper").getAbsolutePath))
      val wrapper = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl), ChirrtlForm), Nil).circuit
      write(s"oracle/$name.wrapper.sfc.fir", wrapper.serialize)
    }
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
  for ((name, memory, hub, count) <- Seq(("fanout", "ram", "Top", 4), ("golden-rocket", "rf", "Rocket", 1), ("grouped-rocket", "rf", "Rocket", 1), ("ready-valid-rocket", "rf", "Rocket", 1))) {
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
    // SFC gathers ModelReadPort annotations through a hash collection. Grouping
    // external channels can permute that collection's read lanes. Preserve the
    // paired command/response identity, and require a complete lane bijection.
    val nativeAdapter = equations(n); val expectedAdapter = equations(s)
    val address = "model.channels.read_cmds\\[([0-9]+)\\].bits.addr".r
    def lanes(eqs: Map[String, String]) = eqs.toSeq.collect {
      case (address(index), value) => value -> index
    }.toMap
    val nl = lanes(nativeAdapter); val sl = lanes(expectedAdapter)
    require(nl.keySet == sl.keySet && nl.values.toSet.size == nl.size && sl.values.toSet.size == sl.size,
      s"$name SRAM read-command identities differ")
    val permutation = sl.map { case (value, index) => index -> nl(value) }
    val lane = "model.channels.read_(cmds|resps)\\[([0-9]+)\\]".r
    def normalizeLane(text: String) = lane.replaceAllIn(text, m =>
      s"model.channels.read_${m.group(1)}[${permutation(m.group(2))}]")
    val adapter = expectedAdapter.map { case (key, value) => normalizeLane(key) -> normalizeLane(value) }
    require(nativeAdapter == adapter, s"$name paired adapter equations differ: " +
      adapter.toSeq.filter { case (key, value) => !nativeAdapter.get(key).contains(value) }.take(3))
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
    val forwardPorts = sAnnos.collect {
      case c: FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[DecoupledForwardChannel] =>
        (c.sources.toSeq.flatten ++ c.sinks.toSeq.flatten).head.ref
    }.toSet
    if (name == "ready-valid-rocket") {
      val wrapper = circuit(s"oracle/$name.wrapper.sfc.fir")
      val sw = module(wrapper, wrapper.main)
      def leaves(port: Port): Seq[(String, (Direction, String))] = {
        def visit(t: Type, path: String, direction: Direction): Seq[(String, (Direction, String))] = t match {
          case BundleType(fields) => fields.flatMap(f => visit(f.tpe, path + "_" + f.name,
            if (f.flip == Flip) { if (direction == Input) Output else Input } else direction))
          case UIntType(IntWidth(w)) if w == 0 => Nil
          case SIntType(IntWidth(w)) if w == 0 => Nil
          case other => Seq(path -> (direction, other.serialize))
        }
        visit(port.tpe, "channelPorts_" + port.name.replace(newHub, oldHub), port.direction)
      }
      val nativePorts = module(native, native.main).ports.filter(p =>
        forwardPorts.exists(f => p.name == f.replace(oldHub, newHub)) ||
        p.name == s"${newHub}_io_dmem_req_ready_sink" || p.name == s"${newHub}_io_imem_resp_ready_source")
      val contracts = nativePorts.flatMap(leaves).toMap
      val reference = abi(sw).filter { case (key, _) => nativePorts.exists(p =>
        key.startsWith("channelPorts_" + p.name.replace(newHub, oldHub) + "_")) }
      require(contracts == reference, s"$name SFC SimWrapper ready/valid ABI differs")
      require(nativePorts.size == 4, s"$name missing forward/reverse wrapper ports")
      def expectedControl(text: String) = text.replace(oldHub, newHub)
        .replace("target.", "target_FAMETop.") match {
          case "clock" => "hostClock"
          case "reset" => "hostReset"
          case other => other
        }
      def nativeControl(text: String): String = {
        if (text.startsWith("target_FAMETop.")) {
          val rest = text.stripPrefix("target_FAMETop.").replace('.', '_')
          "target_FAMETop." + rest
        } else if (nativePorts.exists(p => text.startsWith(p.name + ".")))
          "channelPorts_" + text.replace('.', '_')
        else text
      }
      def controls(eqs: Map[String, String], normalize: String => String) = eqs.collect {
        case (key, value) if (key.contains("ReadyValidChannel_") || value.contains("ReadyValidChannel_")) &&
          !key.contains("_target_bits") && !value.contains("_target_bits") => normalize(key) -> normalize(value)
      }
      val nc = controls(equations(module(native, native.main)), nativeControl)
      val sc = controls(equations(sw), expectedControl)
      require(nc == sc && nc.size == 32, s"$name SimWrapper handshake/reset bindings differ: " +
        sc.toSeq.filter { case (key, value) => !nc.get(key).contains(value) }.take(3))
      println(s"PASS $name: ${contracts.size} flattened SFC SimWrapper port contracts and ${nc.size} control bindings, both directions")
    }
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
        val externalFields = if (forwardPorts.nonEmpty) {
          def restore(value: JValue): JValue = value match {
            case JString(target) if target.startsWith(s"~${expected.main}|${expected.main}>") =>
              JString(forwardPorts.foldLeft(target)((t, port) =>
                t.replace(s">${port.replace(oldHub, newHub)}.bits.bits.", s">${port.replace(oldHub, newHub)}.bits.bits_")))
            case JObject(fields) => JObject(fields.map { case (k, v) => k -> restore(v) })
            case JArray(values) => JArray(values.map(restore))
            case other => other
          }
          compact(render(restore(parse(renamed))))
        } else renamed
        a match {
          // Native transport converts the scalar Clock payload to its single
          // Boolean vector lane; the pre-wrapper SFC endpoint is scalar.
          case c: FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[TargetClockChannel] =>
            externalFields.replace("_clock_sink.bits[0]", "_clock_sink.bits")
          case _ => externalFields
        }
      } else JsonProtocol.serialize(Seq(a)).replace(s"${oldHub}_", s"${newHub}_")
      compact(render(canonical(parse(text))))
    }
    require(multiset(nAnnos.map(a => normalize(a, true))) == multiset(sAnnos.map(a => normalize(a, false))),
      s"$name retained annotation multiset differs")
    println(s"PASS $name: ${equations(s).size} adapter equations, hub/adapter ABIs, $count instances, ${nAnnos.size} annotations, ${referenceHubEquations.size} hub control equations and both XDC files")
  }
}
