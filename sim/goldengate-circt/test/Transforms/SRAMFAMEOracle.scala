// See LICENSE for license details.
// Transform only the independent SFC register-file oracle. Never compile the
// native candidate: parse its hardware and compare equations and metadata.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import scala.collection.mutable
import org.json4s._
import org.json4s.native.JsonMethods._

object SRAMFAMEOracle extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  def write(path: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(directory, path))
    try out.write(text) finally out.close()
  }
  def statements(module: Module): Seq[Statement] = {
    val result = mutable.ArrayBuffer[Statement]()
    def visit(s: Statement): Unit = { result += s; s.foreachStmt(visit) }
    visit(module.body); result.toSeq
  }
  val annotations = JsonProtocol.deserialize(read("oracle/golden-rocket.channels.sfc.json"))
  val state = new ResolveAndCheck().runTransform(CircuitState(
    Parser.parse(read("oracle/golden-rocket.channels.sfc.fir")), LowForm, annotations))
  val analysis = new FAMEChannelAnalysis(state)
  val original = state.circuit.modules.find(_.name == "rf").get.asInstanceOf[Module]
  val added = mutable.ArrayBuffer[Annotation]()
  val oracle = FAMEModuleTransformer(original, analysis, added)
  require(added.isEmpty, "non-hub SRAM must not emit generated-clock XDC")
  write("oracle/golden-rocket.fame.sfc.fir", oracle.serialize)
  val circuit = Parser.parse(read("golden-rocket-fame/post-sram-fame.fir")
    .replaceAll("(?m)^(\\s*)public module ", "$1module "))
  val native = circuit.modules.find(_.name == "rf").get.asInstanceOf[Module]
  def abi(module: Module) = module.ports.map(p => p.name -> (p.direction, p.tpe.serialize)).toMap
  require(abi(native) == abi(oracle), "SRAM Decoupled ABI differs from SFC")
  require(native.ports.take(2).map(_.name) == Seq("hostClock", "hostReset"))
  val inputs = oracle.ports.filter(p => p.direction == Input && p.name.endsWith("_sink")).map(_.name)
  val outputs = oracle.ports.filter(p => p.direction == Output && p.name.endsWith("_source")).map(_.name)
  require(inputs.size == 8 && outputs.size == 2)

  case class Hardware(module: Module) {
    val stmts = statements(module)
    val nodes = stmts.collect { case DefNode(_, name, value) => name -> value }.toMap
    val connects = stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs }.toMap
    val registers = stmts.collect { case r: DefRegister => r.name -> r }.toMap
    val fired = (inputs ++ outputs).map { port =>
      val local = port.stripSuffix("_sink").stripSuffix("_source")
      val matches = registers.values.filter(_.name.startsWith(local + "_fired")).toSeq
      require(matches.size == 1, s"$local has no unique fired register")
      val reg = matches.head
      require(reg.clock.serialize == "hostClock" && reg.reset.serialize == "hostReset")
      require(reg.init.asInstanceOf[UIntLiteral].value == 0, s"virtual channel $local must reset unfired")
      port -> reg.name
    }.toMap
    val firedNames = fired.map { case (port, reg) => reg -> (port + ".fired") }
    def leaf(expression: Expression): String = expression match {
      case r: WRef => firedNames.getOrElse(r.name, r.name)
      case other => other.serialize
    }
    def evaluate(expression: Expression, values: Map[String, BigInt]): BigInt = expression match {
      case UIntLiteral(value, _) => value
      case r: WRef => values.getOrElse(leaf(r),
        evaluate(nodes.getOrElse(r.name, throw new Exception(s"unknown boolean leaf ${r.name}")), values))
      case r: WSubField => values.getOrElse(r.serialize,
        throw new Exception(s"unknown field ${r.serialize}"))
      case Mux(c, t, f, _) => evaluate(if (evaluate(c, values) != 0) t else f, values)
      case DoPrim(PrimOps.AsUInt, Seq(v), _, _) => evaluate(v, values)
      case DoPrim(PrimOps.And, Seq(a, b), _, _) => evaluate(a, values) & evaluate(b, values)
      case DoPrim(PrimOps.Or, Seq(a, b), _, _) => evaluate(a, values) | evaluate(b, values)
      case DoPrim(PrimOps.Not, Seq(a), _, _) => BigInt(1) ^ evaluate(a, values)
      case other => throw new Exception(s"unexpected boolean expression ${other.serialize}")
    }
    // Expanding native SSA nodes exposes the actual memory payload wiring;
    // names of exporter temporaries do not form part of the hardware contract.
    def expression(e: Expression): String = e match {
      case r: WRef if nodes.contains(r.name) => expression(nodes(r.name))
      case r: WRef => leaf(r)
      case other => other.serialize
    }
    val memory = stmts.collect { case m: DefMemory => m }
    val payloadConnections = connects.toSeq.filter { case (lhs, _) =>
      lhs.startsWith("rf.") || outputs.exists(p => lhs == p + ".bits")
    }.map { case (lhs, rhs) => lhs -> expression(rhs) }.toMap
  }
  val sfc = Hardware(oracle); val circt = Hardware(native)
  require(circt.memory == sfc.memory, "SRAM depth/latency/ports/RUW changed")
  require(circt.payloadConnections == sfc.payloadConnections, "SRAM payload or gated memory-clock wiring differs")
  for (port <- inputs ++ outputs; f <- 0 to 1; fired <- 0 to 1; ready <- 0 to 1; valid <- 0 to 1) {
    val values = Map("targetCycleFinishing" -> BigInt(f), (port + ".fired") -> BigInt(fired),
      (port + ".ready") -> BigInt(ready), (port + ".valid") -> BigInt(valid))
    val expected = BigInt(if (f == 1) 0 else fired | (ready & valid))
    require(circt.evaluate(circt.connects(circt.fired(port)), values) == expected)
    require(sfc.evaluate(sfc.connects(sfc.fired(port)), values) == expected)
    if (inputs.contains(port)) {
      val expectedReady = BigInt(f & (1 ^ fired))
      require(circt.evaluate(circt.connects(port + ".ready"), values) == expectedReady)
      require(sfc.evaluate(sfc.connects(port + ".ready"), values) == expectedReady)
    }
  }
  // Exhaustively compare output valid across all eight input valids and both
  // output fired bits, including nondependent write inputs.
  val validVariables = inputs.map(_ + ".valid") ++ outputs.map(_ + ".fired")
  for (mask <- 0 until (1 << validVariables.size)) {
    val values = validVariables.zipWithIndex.map { case (v, i) => v -> BigInt((mask >> i) & 1) }.toMap
    outputs.foreach { port =>
      require(circt.evaluate(circt.connects(port + ".valid"), values) ==
        sfc.evaluate(sfc.connects(port + ".valid"), values), s"$port output dependency differs")
    }
  }
  val finishVariables = inputs.map(_ + ".valid") ++ outputs.flatMap(p => Seq(p + ".fired", p + ".ready", p + ".valid"))
  for (mask <- 0 until (1 << finishVariables.size)) {
    val values = finishVariables.zipWithIndex.map { case (v, i) => v -> BigInt((mask >> i) & 1) }.toMap
    require(circt.evaluate(circt.connects("targetCycleFinishing"), values) ==
      sfc.evaluate(sfc.connects("targetCycleFinishing"), values), "SRAM finishing differs")
  }
  val top = circuit.modules.find(_.name == circuit.main).get.asInstanceOf[Module]
  val topAssignments = statements(top).collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs.serialize }.toMap
  val instances = statements(top).collect { case i: WDefInstance if i.module == "rf" => i }
  require(instances.size == 1)
  val instance = instances.head.name
  (inputs ++ outputs).foreach { port =>
    val topPort = instance + "_" + port
    require(top.ports.exists(p => p.name == topPort && abi(oracle)(port) == (p.direction, p.tpe.serialize)))
    if (inputs.contains(port)) require(topAssignments(s"$instance.$port") == topPort)
    else require(topAssignments(topPort) == s"$instance.$port")
  }
  require(topAssignments(s"$instance.hostClock") == "hostClock")
  require(topAssignments(s"$instance.hostReset") == "hostReset")
  require(!topAssignments.contains(s"$instance.clk"))

  // Apply SFC's own Annotation.update with the SRAM subset of its rename map.
  // Resolve old top bindings from actual SSA-equivalent direct connections.
  // Preparation already has a structured SFC comparison. Use its native
  // spelling here: legal hub instance names differ between compilers. SFC
  // Annotation.update remains the executable rename oracle.
  val beforeCircuit = Parser.parse(read("golden-rocket-candidate/post-sram-channels.fir")
    .replaceAll("(?m)^(\\s*)public module ", "$1module "))
  val beforeAnnotations = JsonProtocol.deserialize(read("golden-rocket-candidate/post-sram-channels-all.json"))
  val oldTop = beforeCircuit.modules.find(_.name == beforeCircuit.main).get.asInstanceOf[Module]
  val oldInstances = statements(oldTop).collect { case i: WDefInstance if i.module == "rf" => i }
  require(oldInstances.size == 1)
  val oldInstance = oldInstances.head.name
  val groups = beforeAnnotations.collect { case g: FAMEChannelPortsAnnotation if g.ports.head.module == "rf" => g }
  val renames = RenameMap()
  groups.foreach { group =>
    require(group.ports.size == 1)
    val old = group.ports.head
    val direction = original.ports.find(_.name == old.ref).get.direction
    val suffix = if (direction == Input) "_sink" else "_source"
    renames.record(old, old.copy(ref = group.localName + suffix).field("bits"))
    val oldInstancePort = s"$oldInstance.${old.ref}"
    val endpoints = statements(oldTop).collect {
      case Connect(_, lhs, rhs) if lhs.serialize == oldInstancePort => rhs.serialize
      case Connect(_, lhs, rhs) if rhs.serialize == oldInstancePort => lhs.serialize
    }
    require(endpoints.size == 1)
    val topTarget = ModuleTarget(state.circuit.main, oldTop.name).ref(endpoints.head)
    renames.record(topTarget, topTarget.copy(ref = instance + "_" + group.localName + suffix).field("bits"))
  }
  val expected = beforeAnnotations.filter {
    case d: firrtl.transforms.DontTouchAnnotation if d.target.module == "rf" => false
    case _: firrtl.transforms.DedupedResult => false
    case _ => true
  }.flatMap(_.update(renames))
  write("oracle/golden-rocket.fame.sfc.json", JsonProtocol.serialize(expected))
  def sorted(value: JValue): JValue = value match {
    case JObject(fields) => JObject(fields.sortBy(_._1).map { case (k, v) => k -> sorted(v) })
    case JArray(values) => JArray(values.map(sorted))
    case other => other
  }
  def annotationMultiset(json: String) = parse(json).asInstanceOf[JArray].arr
    .map(a => compact(render(sorted(a)))).groupMapReduce(identity)(_ => 1)(_ + _)
  require(annotationMultiset(read("golden-rocket-fame/post-sram-fame-all.json")) ==
    annotationMultiset(JsonProtocol.serialize(expected)), "SRAM payload annotation transfer differs from SFC")
  println("PASS golden Rocket rf: 10 Decoupled ABIs, memory payload/gate wiring, 160 fired transitions, " +
    "2048 output-valid cases, 16384 finishing cases and SFC annotation renames match")
}
