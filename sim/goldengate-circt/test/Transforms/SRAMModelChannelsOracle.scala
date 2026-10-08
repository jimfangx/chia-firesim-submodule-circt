// See LICENSE for license details.
// Compare the optional SRAM preparation boundary with the unchanged SFC passes.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.fame._
import midas.targetutils.FirrtlMemModelAnnotation
import firesim.lib.bridgeutils.RationalClock

object SRAMModelChannelsOracle extends App {
  val directory = new java.io.File(args(0)); directory.mkdirs()
  // Reuse the multiport/fanout probe and immutable Rocket LowForm preparation.
  ExtractSRAMModelsOracle.main(args)
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, name))
    try out.write(text) finally out.close()
  }
  for ((name, main, parent, memory) <- Seq(
      ("fanout", "Top", "Parent", "ram"),
      ("golden-rocket", "Rocket", "Rocket", "rf"))) {
    val source = scala.io.Source.fromFile(new java.io.File(directory, name + ".fir"))
    val parsed = try Parser.parse(source.mkString) finally source.close()
    // Select the real Rocket hierarchy from the golden circuit as the hub.
    // This isolates its SRAM channels from unrelated bridge infrastructure.
    val modules = parsed.modules.map(m => m.name -> m).toMap
    val reachable = scala.collection.mutable.Set[String]()
    def visit(module: String): Unit = if (reachable.add(module)) {
      def instances(stmt: Statement): Unit = stmt match {
        case instance: WDefInstance => visit(instance.module)
        case other => other.foreachStmt(instances)
      }
      modules(module) match {
        case m: Module => instances(m.body)
        case _ =>
      }
    }
    visit(main)
    val complete = name == "golden-rocket" && args.lift(2).contains("complete")
    val circuit = parsed.copy(main = main, modules = parsed.modules.filter(m => reachable(m.name)).map {
      case m: Module if complete && m.name == main => m.copy(
        ports = m.ports :+ Port(NoInfo, "external_clock", Output, ClockType),
        body = Block(Seq(m.body, Connect(NoInfo, WRef("external_clock", ClockType, PortKind, SinkFlow),
          WRef("clock", ClockType, PortKind, SourceFlow)))))
      case m => m
    })
    val clockChannel = FAMEChannelConnectionAnnotation.sink(
      "targetClock", TargetClockChannel(Seq(RationalClock("base", 1, 1)), Seq(1)),
      None, Seq(ModuleTarget(main, main).ref("clock")))
    // A complete Rocket probe also supplies its external token channels. Keep
    // the historical SRAM-only boundary available for the narrower comparison.
    val external = if (complete) {
      circuit.modules.find(_.name == main).get.ports.filterNot(_.tpe == ClockType).map { p =>
        val endpoint = ModuleTarget(main, main).ref(p.name)
        // Bridge-facing channels identify a source clock exported by the hub.
        val clock = Some(ModuleTarget(main, main).ref("external_clock"))
        if (p.direction == Input)
          FAMEChannelConnectionAnnotation.sink("external_" + p.name, PipeChannel(0), clock, Seq(endpoint))
        else FAMEChannelConnectionAnnotation.source("external_" + p.name, PipeChannel(0), clock, Seq(endpoint))
      }
    } else Seq.empty
    val input = CircuitState(circuit, LowForm, Seq(
      FirrtlMemModelAnnotation(ModuleTarget(main, parent).ref(memory)), clockChannel) ++ external)
    val wrapped = new LowFirrtlCompiler().compile(WrapTop.runTransform(input), Nil)
    write(name + ".channels-input.fir", wrapped.circuit.serialize)
    write(name + ".channels-input.json", JsonProtocol.serialize(wrapped.annotations))
    val labeled = new LabelSRAMModels().runTransform(wrapped)
    val promoted = new ExtractModel().runTransform(labeled)
    val lowered = new LowFirrtlCompiler().compile(promoted, Nil)
    // The Scala passthrough transform creates untyped WRefs; Midas resolves
    // their kinds/types again before FAMEDefaults pattern-matches instances.
    val passthrough = new ResolveAndCheck().runTransform(
      PromotePassthroughConnections.runTransform(lowered))
    val defaults = new FAMEDefaults().runTransform(passthrough)
    val clocked = FindDefaultClocks.runTransform(defaults)
    val excised = new ChannelExcision().runTransform(clocked)
    val inferred = new InferModelPorts().runTransform(excised)
    write(name + ".channels.sfc.fir", inferred.circuit.serialize)
    write(name + ".channels.sfc.json", JsonProtocol.serialize(inferred.annotations))
    println(s"PASS SFC $name SRAM clocks, channel excision, and model port inference")
  }
}

// Compiler differential comparison, preserving annotation multiplicity and
// payload order. Resolve excised top ports back to model instance/port identity
// so SFC's legal hub-instance disambiguation cannot mask a wiring mismatch.
object SRAMModelChannelsCompare extends App {
  import org.json4s._
  import org.json4s.native.JsonMethods._
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  for ((name, hub) <- Seq("fanout" -> "Top", "golden-rocket" -> "Rocket")) {
    val input = Parser.parse(read(s"oracle/$name.channels-input.fir"))
    val originalPorts = input.modules.find(_.name == input.main).get.ports.map(_.name).toSet
    def boundary(fir: String, json: String) = {
      // The analysis boundary exports FIRRTL 1.2 expressions and statements.
      // CIRCT still prints module visibility, absent from the pinned parser's
      // grammar. Ignore only that keyword; never compile the native candidate.
      val circuit = Parser.parse(read(fir).replaceAll("(?m)^(\\s*)public module ", "$1module "))
      val top = circuit.modules.find(_.name == circuit.main).get.asInstanceOf[Module]
      val instances = scala.collection.mutable.Map[String, String]()
      def visit(stmt: Statement)(f: Statement => Unit): Unit = {
        f(stmt); stmt.foreachStmt(s => visit(s)(f))
      }
      visit(top.body) {
        case i: WDefInstance => instances(i.name) = i.module
        case _ =>
      }
      // Resolve actual instance bindings without treating distinct alias
      // outputs as equivalent. Promotion must select the same source port.
      def identity(instance: String, port: String): String =
        s"~${circuit.main}|${instances(instance)}/${if (instances(instance) == hub) "hub" else instance}>$port"
      val bindings = scala.collection.mutable.Map[String, String]()
      def bind(port: String, instance: String, childPort: String): Unit = {
        val key = s"~${circuit.main}|${circuit.main}>$port"
        assert(!bindings.contains(key), s"ambiguous top-port binding: $key")
        bindings(key) = identity(instance, childPort)
      }
      visit(top.body) {
        case Connect(_, WRef(p, _, _, _), WSubField(WRef(i, _, _, _), q, _, _)) => bind(p, i, q)
        case Connect(_, WSubField(WRef(i, _, _, _), q, _, _), WRef(p, _, _, _)) => bind(p, i, q)
        case _ =>
      }
      def normalize(value: JValue): JValue = value match {
        case JString(target) => JString(bindings.getOrElse(target, target))
        case JArray(values) => JArray(values.map(normalize))
        case JObject(fields) => JObject(fields.map { case (key, member) => key -> normalize(member) }.sortBy(_._1))
        case other => other
      }
      val records = parse(read(json)).asInstanceOf[JArray].arr.filterNot(record =>
        (record \ "class") == JString("firrtl.transforms.DedupedResult")).map { record =>
        val normalized = normalize(record)
        // A default pipe name is generated from physical instance spellings.
        // Compare its source/sink-derived identity as well as all endpoint,
        // clock, channelInfo, and module-port records.
        if ((record \ "channelInfo" \ "class") == JString("midas.passes.fame.PipeChannel") &&
            (record \ "sources") != JNothing && (record \ "sinks") != JNothing)
          normalized.removeField { case (key, _) => key == "globalName" }
        else normalized
      }
      val annotations = records.map(r => compact(render(r))).groupBy(x => x).view.mapValues(_.size).toMap
      val ports = top.ports.map { p =>
        // Original top-level I/O retains its ABI name. Only newly excised
        // channel ports may use a compiler-generated instance prefix.
        (if (originalPorts(p.name)) p.name else "channel",
          bindings.getOrElse(s"~${circuit.main}|${circuit.main}>${p.name}", p.name), p.direction, p.tpe.serialize)
      }.groupBy(x => x).view.mapValues(_.size).toMap
      val models = instances.map { case (instance, module) =>
        (if (module == hub) "hub" else instance) -> module
      }.toMap
      val ramModules = records.collect {
        case record if (record \ "class") == JString("firrtl.transforms.NoDedupAnnotation") =>
          (record \ "target").asInstanceOf[JString].s.split('|').last
      }.toSet
      val memories = circuit.modules.collect { case module: Module if ramModules(module.name) =>
        val declarations = scala.collection.mutable.ArrayBuffer[String]()
        visit(module.body) {
          case mem: DefMemory => declarations += Seq(mem.name, mem.dataType.serialize, mem.depth,
            mem.readLatency, mem.writeLatency, mem.readers, mem.writers, mem.readwriters, mem.readUnderWrite).mkString("|")
          case _ =>
        }
        module.name -> (module.ports.map(p => (p.name, p.direction, p.tpe.serialize)), declarations.toSeq)
      }.toMap
      (annotations, ports, models, memories, records)
    }
    val sfc = boundary(s"oracle/$name.channels.sfc.fir", s"oracle/$name.channels.sfc.json")
    val native = boundary(s"$name-candidate/post-sram-channels.fir", s"$name-candidate/post-sram-channels-all.json")
    assert(sfc._1 == native._1, s"$name annotation mismatch: missing=${sfc._1.toSet -- native._1.toSet}, extra=${native._1.toSet -- sfc._1.toSet}")
    assert(sfc._2 == native._2, s"$name top port directions/types/bindings differ: missing=${sfc._2.toSet -- native._2.toSet}, extra=${native._2.toSet -- sfc._2.toSet}")
    assert(sfc._3 == native._3, s"$name promoted models differ")
    assert(sfc._4 == native._4, s"$name SRAM ports or memory semantics differ")
    val counts = sfc._5.groupBy(r => (r \ "class").asInstanceOf[JString].s).view.mapValues(_.size).toMap
    println(s"PASS $name: ${sfc._3.size} models, ${sfc._2.values.sum} top ports, ${counts("midas.passes.fame.FAMEChannelConnectionAnnotation")} channels, ${counts("midas.passes.fame.FAMEChannelPortsAnnotation")} local groups; all ${sfc._5.size} annotation records match")
  }
}
