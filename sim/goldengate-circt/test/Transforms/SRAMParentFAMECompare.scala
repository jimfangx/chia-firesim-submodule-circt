// See LICENSE for license details.
// Inspect the native pre-queue boundary; never run SFC on the candidate.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMParentFAMECompare extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  def circuit(path: String) = Parser.parse(read(path)
    .replaceAll("(?m)^(\\s*)public module ", "$1module "))
  def statements(module: Module): Seq[Statement] = {
    val result = scala.collection.mutable.ArrayBuffer[Statement]()
    def visit(s: Statement): Unit = { result += s; s.foreachStmt(visit) }
    visit(module.body); result.toSeq
  }
  case class Hardware(module: Module) {
    val stmts = statements(module)
    val nodes = stmts.collect { case DefNode(_, n, v) => n -> v }.toMap
    val connects = stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs }.toMap
    val registers = stmts.collect { case r: DefRegister => r.name -> r }.toMap
    def canonical(e: Expression): String = e match {
      case UIntLiteral(v, _) => v.toString
      case r: WRef if nodes.contains(r.name) => canonical(nodes(r.name))
      case DoPrim(PrimOps.AsUInt, Seq(v), _, _) => canonical(v)
      case DoPrim(op, Seq(a, b), _, _) if op == PrimOps.And || op == PrimOps.Or =>
        def operands(v: Expression): Seq[String] = v match {
          case r: WRef if nodes.contains(r.name) => operands(nodes(r.name))
          case DoPrim(same, Seq(x, y), _, _) if same == op => operands(x) ++ operands(y)
          case other => Seq(canonical(other))
        }
        val vs = (operands(a) ++ operands(b)).distinct.sorted
        val zero = if (op == PrimOps.And) "0" else "1"
        val one = if (op == PrimOps.And) "1" else "0"
        if (vs.contains(zero)) zero else {
          val terms = vs.filterNot(_ == one)
          if (terms.isEmpty) one else if (terms.size == 1) terms.head
          else s"$op(${terms.mkString(",")})"
        }
      case DoPrim(PrimOps.Not, Seq(v), _, _) => canonical(v) match {
        case "0" => "1"
        case "1" => "0"
        case other => s"not($other)"
      }
      case Mux(c, t, f, _) => s"mux(${canonical(c)},${canonical(t)},${canonical(f)})"
      case other => other.serialize
    }
  }
  val native = circuit("golden-rocket-parent-fame/post-sram-parent-fame.fir")
  val sfc = circuit("oracle/golden-rocket.transport-fame.sfc.fir")
  def module(c: Circuit, name: String) = c.modules.find(_.name == name).get.asInstanceOf[Module]
  def abi(m: Module) = m.ports.map(p => p.name -> (p.direction, p.tpe.serialize.replace(" ", ""))).toMap
  val ram = module(native, "rf"); val expectedRam = module(sfc, "rf")
  require(abi(ram) == abi(expectedRam), "golden rf Decoupled ABI differs")
  require(statements(ram).collect { case m: DefMemory => m.copy(info = NoInfo) } ==
    statements(expectedRam).collect { case m: DefMemory => m.copy(info = NoInfo) }, "golden rf memory semantics differ")
  println("PASS golden rf: ten channel ABIs and all memory semantic fields")
  val actualABI = abi(module(native, "Rocket")); val expectedABI = abi(module(sfc, "Rocket"))
  require(actualABI == expectedABI, s"Rocket port ABI differs: missing=${expectedABI.toSet -- actualABI.toSet}; extra=${actualABI.toSet -- expectedABI.toSet}")
  println(s"PASS all ${expectedABI.size} parent ports: names, directions and complete types match SFC")

  // transformTop connects whole Decoupled bundles, including flipped ready.
  // Checking payload annotations alone would miss a scalar-only passthrough.
  val actualTop = Hardware(module(native, native.main))
  val expectedTop = Hardware(module(sfc, sfc.main))
  val actualTopABI = abi(actualTop.module); val expectedTopABI = abi(expectedTop.module)
  for ((source, sink, oldSource, oldSink) <- Seq(
      ("external_io_fpu_hartid_source", "external_io_hartid_sink", "io_fpu_hartid", "io_hartid"),
      ("external_io_fpu_ll_resp_data_source", "external_io_dmem_resp_bits_data_sink", "io_fpu_ll_resp_data", "io_dmem_resp_bits_data"))) {
    for (port <- Seq(source, sink))
      require(actualTopABI.get(port) == expectedTopABI.get(port), s"passthrough $port Decoupled ABI differs")
    require(!actualTopABI.contains(oldSource) && !actualTopABI.contains(oldSink), "stale passthrough scalar port remains")
    require(actualTop.connects(source).serialize == sink &&
      actualTop.connects(source).serialize == expectedTop.connects(source).serialize,
      s"$source payload/valid/ready passthrough differs")
  }
  println("PASS two top passthroughs: Decoupled ABI and complete payload/valid/ready connections match SFC")

  // Expand emitted reduction nodes, preserving condition multiplicity.
  // Every SFC condition, including clock valid, must remain identical.
  val actualHW = Hardware(module(native, "Rocket"))
  val expectedHW = Hardware(module(sfc, "Rocket"))
  def completionTerms(h: Hardware, e: Expression): Seq[String] = e match {
    case r: WRef if h.nodes.contains(r.name) => completionTerms(h, h.nodes(r.name))
    case DoPrim(PrimOps.And, Seq(a, b), _, _) => completionTerms(h, a) ++ completionTerms(h, b)
    case other => Seq(h.canonical(other))
  }
  for (key <- Seq("targetCycleFinishing", "clock_sink.ready")) {
    val actual = completionTerms(actualHW, actualHW.connects(key))
    val expected = completionTerms(expectedHW, expectedHW.connects(key))
    require(actual.sorted == expected.sorted, s"$key completion conditions differ: missing=${expected.diff(actual)}; extra=${actual.diff(expected)}")
    println(s"PASS $key: all ${expected.size} SFC conditions and their multiplicities match")
  }

  def paths(c: Circuit, json: String): Seq[JValue] = {
    val top = module(c, c.main)
    val stmts = statements(top)
    val instances = stmts.collect { case i: WDefInstance => i.name -> i.module }.toMap
    val bindings = scala.collection.mutable.Map[String, Set[String]]()
    def bind(port: String, instance: String, childPort: String): Unit = {
      val identity = s"~${c.main}|${instances(instance)}>$childPort"
      bindings(port) = bindings.getOrElse(port, Set.empty) + identity
    }
    stmts.foreach {
      case Connect(_, WRef(p, _, _, _), WSubField(WRef(i, _, _, _), q, _, _)) => bind(p, i, q)
      case Connect(_, WSubField(WRef(i, _, _, _), q, _, _), WRef(p, _, _, _)) => bind(p, i, q)
      case _ =>
    }
    val prefix = s"~${c.main}|${c.main}>"
    def normalize(v: JValue): JValue = v match {
      case JString(t) if t.startsWith(prefix) =>
        val local = t.stripPrefix(prefix); val port = local.takeWhile(_ != '.')
        val identities = bindings.getOrElse(port, Set(prefix + port))
        require(identities.size == 1, s"ambiguous annotation payload binding $port")
        JString(identities.head + local.drop(port.size))
      case JArray(vs) => JArray(vs.map(normalize))
      case JObject(fs) => JObject(fs.sortBy(_._1).map { case (k, x) => k -> normalize(x) })
      case other => other
    }
    parse(read(json)).asInstanceOf[JArray].arr.filter(a =>
      (a \ "class") == JString("firrtl.transforms.CombinationalPath")).map(normalize)
  }
  val expectedPaths = paths(sfc, "oracle/golden-rocket.transport-fame.sfc.json")
  val actualPaths = paths(native, "golden-rocket-parent-fame/post-sram-parent-fame-all.json")
  require(expectedPaths.size == 39 && actualPaths.size == 39, "path annotation multiplicity differs")
  val matches = expectedPaths.intersect(actualPaths)
  require(matches.size == 39, s"path transfer mismatch: SFC=${expectedPaths.diff(actualPaths)}; native=${actualPaths.diff(expectedPaths)}")
  println("PASS 39/39 CombinationalPath records: actual model bindings, ordered sources, payload targets and multiplicity")

  val transport = circuit("golden-rocket-transport/post-sram-transport.fir")
  val wrapper = circuit("oracle/golden-rocket.transport-wrapper.sfc.fir")
  val nativeWrapper = Hardware(module(transport, transport.main))
  val sfcWrapper = Hardware(module(wrapper, wrapper.main))
  def queues(h: Hardware) = h.stmts.collect {
    case i: WDefInstance if i.module.startsWith("GGFAMEPipe") || i.module.startsWith("PipeChannel") => i
  }
  val actualQueues = queues(nativeWrapper); val expectedQueues = queues(sfcWrapper)
  require(actualQueues.size == 524 && expectedQueues.size == 524, "complete Rocket pipe multiplicity differs")
  def payloads(c: Circuit, qs: Seq[WDefInstance]) = qs.map(q =>
    c.modules.find(_.name == q.module).get.ports.find(_.name == "io_in_bits").get.tpe.serialize)
    .groupMapReduce(identity)(_ => 1)(_ + _)
  require(payloads(transport, actualQueues) == payloads(wrapper, expectedQueues), "complete Rocket queue payload widths differ")
  println("PASS complete Rocket transport: 524 queues and their payload-type multiplicities match production SimWrapper")

  def topIdentities(h: Hardware): Map[String, String] = {
    val instances = h.stmts.collect { case i: WDefInstance => i.name -> i.module }.toMap
    h.module.ports.map { p =>
      val binding = h.connects.get(p.name).map(_.serialize).orElse(h.connects.collectFirst {
        case (lhs, rhs) if rhs.serialize == p.name => lhs
      })
      val child = binding.toSeq.map(_.split("\\.")).find(x => x.length == 2 && instances.contains(x(0)))
      p.name -> child.map(x => "model." + instances(x(0)) + "." + x(1)).getOrElse("top." + p.name)
    }.toMap
  }
  def normalized(text: String, identities: Map[String, String]): String = {
    val flat = queueIdentity(text).replaceAll("(?<![A-Za-z0-9_.])target\\.([A-Za-z0-9_]+)_(bits|valid|ready)", "target.$1.$2")
      .replaceAll("(?<![A-Za-z0-9_.])channelPorts_([A-Za-z0-9_]+)_(bits|valid|ready)", "bridge.$1.$2")
      .replace("target_FAMETop.", "target.")
    val external = "(?<![A-Za-z0-9_.])([A-Za-z0-9_]+)\\.(bits|valid|ready)".r.replaceAllIn(flat, m =>
      if (identities.contains(m.group(1))) "bridge." + m.matched else m.matched)
    val resolved = "(?<![A-Za-z0-9_.])bridge\\.([A-Za-z0-9_]+)\\.(bits|valid|ready)".r.replaceAllIn(external, m =>
      "bridge." + identities.getOrElse(m.group(1),
        throw new IllegalArgumentException(s"unbound bridge port ${m.group(1)} in $text / $external")) + "." + m.group(2))
    "(?<![A-Za-z0-9_.])target\\.([A-Za-z0-9_]+)\\.(bits|valid|ready)".r.replaceAllIn(resolved, m =>
      identities(m.group(1)) + "." + m.group(2))
  }
  // SFC disambiguates the hub instance Rocket as Rocket_; pipe names embed
  // that physical spelling. Endpoint comparison still uses model identities.
  def queueIdentity(name: String): String =
    name.replace("PipeChannel_Rocket__rf_", "PipeChannel_Rocket_rf_")
      .replace("__to__Rocket__rf_", "__to__Rocket_rf_")
  val nids = topIdentities(actualTop); val sids = topIdentities(expectedTop)
  val actualByName = actualQueues.map(q => queueIdentity(q.name) -> q).toMap
  val expectedByName = expectedQueues.map(q => queueIdentity(q.name) -> q).toMap
  require(actualByName.size == 524 && expectedByName.size == 524 &&
    actualByName.keySet == expectedByName.keySet,
    s"pipe identities differ or collide: missing=${expectedByName.keySet -- actualByName.keySet}; extra=${actualByName.keySet -- expectedByName.keySet}")
  for (q <- expectedByName.keys.toSeq.sorted) {
    val nq = actualByName(q); val sq = expectedByName(q)
    require(module(transport, nq.module).ports.find(_.name == "io_in_bits").get.tpe ==
      module(wrapper, sq.module).ports.find(_.name == "io_in_bits").get.tpe,
      s"$q payload type differs")
    for (field <- Seq("io_in_bits", "io_in_valid", "io_out_ready")) {
      val actual = normalized(nativeWrapper.canonical(nativeWrapper.connects(nq.name + "." + field)), nids)
      val expected = normalized(sfcWrapper.canonical(sfcWrapper.connects(sq.name + "." + field)), sids)
      require(actual == expected, s"$q.$field transport equation differs: native=$actual; SFC=$expected")
    }
    for (field <- Seq("io_out_bits", "io_out_valid")) {
      def destinations(h: Hardware, name: String, ids: Map[String, String]) = h.connects.collect {
        case (lhs, rhs) if rhs.serialize == name + "." + field => normalized(lhs, ids)
      }.toSet
      val actual = destinations(nativeWrapper, nq.name, nids)
      val expected = destinations(sfcWrapper, sq.name, sids)
      // Earlier bridge alias queues have no output consumer after SFC
      // resolves last-connect semantics on their shared wrapper output.
      require(actual == expected, s"$q/$field destinations differ: native=$actual; SFC=$expected")
    }
    require(nativeWrapper.connects(nq.name + ".clock").serialize == "hostClock" &&
      nativeWrapper.connects(nq.name + ".reset").serialize == "hostReset" &&
      sfcWrapper.connects(sq.name + ".clock").serialize == "clock" &&
      sfcWrapper.connects(sq.name + ".reset").serialize == "reset", s"$q host controls differ")
  }
  for (port <- Seq("external_io_hartid_sink", "external_io_dmem_resp_bits_data_sink"))
    require(normalized(nativeWrapper.canonical(nativeWrapper.connects(port + ".ready")), nids) ==
      normalized(sfcWrapper.canonical(sfcWrapper.connects("channelPorts_" + port + "_ready")), sids), s"$port fanout ready differs")
  def modelSourceReadies(h: Hardware, ids: Map[String, String]) = h.connects.toSeq.flatMap {
    case (lhs, rhs) =>
      val key = normalized(lhs, ids)
      if (key.startsWith("model.") && key.endsWith(".ready"))
        Some(key -> normalized(h.canonical(rhs), ids))
      else None
  }.toMap
  val actualReadies = modelSourceReadies(nativeWrapper, nids)
  val expectedReadies = modelSourceReadies(sfcWrapper, sids)
  require(actualReadies == expectedReadies,
    s"model-source ready reductions differ: missing=${expectedReadies.toSet -- actualReadies.toSet}; extra=${actualReadies.toSet -- expectedReadies.toSet}")
  println(s"PASS all ${expectedReadies.size} model-source ready reductions, including output-alias fanout")
  println("PASS all 524 queues: payload, valid, ready, destinations and host-control equations match SFC")
}
