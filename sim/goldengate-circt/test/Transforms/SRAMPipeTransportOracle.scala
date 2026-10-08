// See LICENSE for license details.
// Run the unchanged production FAME and SimWrapper on independently prepared
// SRAM handoffs. The candidate is only parsed, never compiled by SFC.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.core.{SimWrapper, SimWrapperConfig}
import org.chipsalliance.cde.config.Parameters
import scala.collection.mutable

object SRAMPipeTransportOracle extends App {
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
  // The isolated Rocket preparation intentionally omits non-SRAM channels.
  // It is covered by SRAMFAMEOracle, not a complete parent transport oracle.
  for ((name, hub, ram, count) <- Seq(("fanout", "Top", "ram", 4))) {
    val input = new ResolveAndCheck().runTransform(CircuitState(
      Parser.parse(read(s"oracle/$name.channels.sfc.fir")), LowForm,
      JsonProtocol.deserialize(read(s"oracle/$name.channels.sfc.json"))))
    val fame = new FAMETransform().runTransform(input)
    write(s"oracle/$name.transport-fame.sfc.fir", fame.circuit.serialize)
    write(s"oracle/$name.transport-fame.sfc.json", JsonProtocol.serialize(fame.annotations))
    implicit val parameters: Parameters = Parameters.empty
    val top = fame.circuit.modules.find(_.name == fame.circuit.main).get
    val types = top.ports.map(p => ModuleTarget(fame.circuit.main, top.name).ref(p.name) -> p).toMap
    val chirrtl = (new chisel3.stage.ChiselStage).emitChirrtl(
      new SimWrapper(SimWrapperConfig(fame.annotations, types)),
      Array("--target-dir", new java.io.File(directory, "oracle").getAbsolutePath))
    val wrapper = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl), ChirrtlForm), Nil).circuit
    write(s"oracle/$name.transport-wrapper.sfc.fir", wrapper.serialize)
    println(s"SFC $name: full parent/SRAM FAME and production SimWrapper emitted ($count SRAM instances)")
  }
}

object SRAMPipeTransportCompare extends App {
  val directory = new java.io.File(args(0))
  def read(path: String): String = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  def circuit(path: String) = Parser.parse(read(path)
    .replaceAll("(?m)^(\\s*)public module ", "$1module "))
  def statements(module: Module): Seq[Statement] = {
    val result = mutable.ArrayBuffer[Statement]()
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
  val native = circuit("fanout-transport/post-sram-transport.fir")
  val sfc = circuit("oracle/fanout.transport-fame.sfc.fir")
  def module(c: Circuit, n: String) = c.modules.find(_.name == n).get.asInstanceOf[Module]
  for (name <- Seq("ram", "Top")) {
    val n = Hardware(module(native, name)); val s = Hardware(module(sfc, name))
    def abi(m: Module) = m.ports.map(p => p.name -> (p.direction, p.tpe.serialize.replace(" ", ""))).toMap
    require(abi(n.module) == abi(s.module), s"$name ABI differs")
    require(n.registers.keySet == s.registers.keySet, s"$name FAME register identities differ")
    for ((reg, expected) <- s.registers) {
      val actual = n.registers(reg)
      require(actual.clock.serialize == expected.clock.serialize &&
        actual.reset.serialize == expected.reset.serialize &&
        n.canonical(actual.init) == s.canonical(expected.init), s"$name/$reg reset/clock differs")
      require(n.canonical(n.connects(reg)) == s.canonical(s.connects(reg)), s"$name/$reg transition differs")
    }
    val equations = s.connects.keySet.filter(k => k.endsWith(".ready") || k.endsWith(".valid") ||
      k == "targetCycleFinishing" || k.endsWith("_buffer.CE") || k.endsWith("_buffer.I"))
    for (key <- equations)
      require(n.canonical(n.connects(key)) == s.canonical(s.connects(key)), s"$name/$key equation differs")
    if (name == "ram") {
      require(n.stmts.collect { case m: DefMemory => m.copy(info = NoInfo) } ==
        s.stmts.collect { case m: DefMemory => m.copy(info = NoInfo) }, "memory semantics changed")
      for (key <- s.connects.keys.filter(k => k.startsWith("ram.") || k.endsWith("_source.bits")))
        require(n.canonical(n.connects(key)) == s.canonical(s.connects(key)), s"SRAM payload $key differs")
    }
    println(s"PASS $name: ABI, ${s.registers.size} reset/transition rules, ${equations.size} ready/valid/finish/gate equations")
  }
  val wrapper = module(native, native.main)
  val target = module(native, "FAMETop")
  val expectedWrapperCircuit = circuit("oracle/fanout.transport-wrapper.sfc.fir")
  val expectedWrapper = module(expectedWrapperCircuit, expectedWrapperCircuit.main)
  val queues = statements(wrapper).collect { case i: WDefInstance if i.module.startsWith("GGFAMEPipe") => i }
  val expectedQueues = statements(expectedWrapper).collect { case i: WDefInstance if i.module.startsWith("PipeChannel") => i }
  require(queues.size == 52 && expectedQueues.size == queues.size, "both-ended queue multiplicity differs")
  require(wrapper.ports.map(_.name).toSet == Set("hostClock", "hostReset", "Top_clock_sink"), "internal pipes leaked to wrapper interface")
  require(expectedWrapper.ports.size == 5, "SFC wrapper has unexpected external data channels")
  val inner = Hardware(target)
  val instances = inner.stmts.collect { case i: WDefInstance => i.name -> i.module }.toMap
  def endpoint(port: String): String = {
    val binding = inner.connects.get(port).map(_.serialize).getOrElse {
      inner.connects.collectFirst { case (lhs, rhs) if rhs.serialize == port => lhs }.get
    }
    val split = binding.split("\\.")
    require(split.length == 2, s"non-direct model endpoint $binding")
    val instance = if (instances(split(0)) == "Top") "hub" else split(0)
    instance + "." + split(1)
  }
  val nativeWiring = Hardware(wrapper)
  // Every queue owns exactly one source and one sink, with host controls and
  // matching payload/handshake fields. Compare the complete endpoint multiset
  // to SFC's retained channel descriptions, ignoring generated global names.
  val pairs = queues.map { q =>
    def source(field: String) = nativeWiring.canonical(nativeWiring.connects(q.name + ".io_in_" + field))
    def sink(field: String) = nativeWiring.connects.collectFirst {
      case (lhs, rhs) if rhs.serialize == q.name + ".io_out_" + field => lhs
    }.get
    val from = source("bits").stripPrefix("target_FAMETop.").stripSuffix(".bits")
    val to = sink("bits").stripPrefix("target_FAMETop.").stripSuffix(".bits")
    require(source("valid") == s"target_FAMETop.$from.valid", s"${q.name} enqueue valid")
    require(nativeWiring.canonical(nativeWiring.connects(q.name + ".io_out_ready")) == s"target_FAMETop.$to.ready", s"${q.name} dequeue ready")
    require(nativeWiring.connects(s"target_FAMETop.$from.ready").serialize == q.name + ".io_in_ready", s"${q.name} source ready")
    require(nativeWiring.connects(s"target_FAMETop.$to.valid").serialize == q.name + ".io_out_valid", s"${q.name} sink valid")
    require(sourceControl(q.name, "clock") == "hostClock" && sourceControl(q.name, "reset") == "hostReset", "queue host controls")
    endpoint(from) -> endpoint(to)
  }
  def sourceControl(q: String, field: String) = nativeWiring.canonical(nativeWiring.connects(q + "." + field))
  val annotations = JsonProtocol.deserialize(read("oracle/fanout.transport-fame.sfc.json"))
  val expectedInner = Hardware(module(sfc, sfc.main))
  val expectedInstances = expectedInner.stmts.collect { case i: WDefInstance => i.name -> i.module }.toMap
  def expectedEndpoint(rt: ReferenceTarget): String = {
    val expr = expectedInner.connects.get(rt.ref).map(_.serialize).getOrElse {
      expectedInner.connects.collectFirst { case (lhs, rhs) if rhs.serialize == rt.ref => lhs }.get
    }
    val split = expr.split("\\.")
    val instance = if (expectedInstances(split(0)) == "Top") "hub" else split(0)
    instance + "." + split(1)
  }
  val expectedPairs = annotations.collect {
    case c: FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[PipeChannel] =>
      require(c.sources.get.size == 1 && c.sinks.get.size == 1)
      expectedEndpoint(c.sources.get.head) -> expectedEndpoint(c.sinks.get.head)
  }
  def multiset[A](values: Seq[A]) = values.groupMapReduce(identity)(_ => 1)(_ + _)
  require(multiset(pairs) == multiset(expectedPairs), "queue endpoint multiset differs")
  val expectedWiring = Hardware(expectedWrapper)
  val wrapperPairs = expectedQueues.map { q =>
    def source(field: String) = expectedWiring.canonical(expectedWiring.connects(q.name + ".io_in_" + field))
    def sink(field: String) = expectedWiring.connects.collectFirst {
      case (lhs, rhs) if rhs.serialize == q.name + ".io_out_" + field => lhs
    }.get
    val from = source("bits").stripPrefix("target.").stripSuffix("_bits")
    val to = sink("bits").stripPrefix("target.").stripSuffix("_bits")
    require(source("valid") == s"target.${from}_valid", "SFC queue source valid")
    require(expectedWiring.canonical(expectedWiring.connects(q.name + ".io_out_ready")) == s"target.${to}_ready", "SFC queue sink ready")
    require(expectedWiring.connects(s"target.${from}_ready").serialize == q.name + ".io_in_ready", "SFC queue source ready")
    require(expectedWiring.connects(s"target.${to}_valid").serialize == q.name + ".io_out_valid", "SFC queue sink valid")
    require(expectedWiring.connects(q.name + ".clock").serialize == "clock" &&
      expectedWiring.connects(q.name + ".reset").serialize == "reset", "SFC queue host controls")
    expectedEndpoint(ModuleTarget(sfc.main, sfc.main).ref(from)) ->
      expectedEndpoint(ModuleTarget(sfc.main, sfc.main).ref(to))
  }
  require(multiset(pairs) == multiset(wrapperPairs), "production SimWrapper queue wiring differs")
  def queueTypes(c: Circuit, qs: Seq[WDefInstance], ps: Seq[(String, String)]) =
    qs.zip(ps).map { case (q, pair) =>
      val payload = c.modules.find(_.name == q.module).get.ports.find(_.name == "io_in_bits").get.tpe.serialize
      pair -> payload
    }
  require(multiset(queueTypes(native, queues, pairs)) ==
    multiset(queueTypes(expectedWrapperCircuit, expectedQueues, wrapperPairs)), "queue payload widths differ")
  require(queues.forall(_.module.endsWith("_L0")) && annotations.collect {
    case c: FAMEChannelConnectionAnnotation if c.channelInfo.isInstanceOf[PipeChannel] =>
      c.channelInfo.asInstanceOf[PipeChannel].latency
  }.forall(_ == 0), "queue latency differs")
  require(nativeWiring.connects("target_FAMETop.Top_clock_sink.bits").serialize ==
    "asClock(Top_clock_sink.bits[0])", "Boolean packet clock conversion differs")
  require(nativeWiring.connects("target_FAMETop.Top_clock_sink.valid").serialize == "Top_clock_sink.valid" &&
    nativeWiring.connects("Top_clock_sink.ready").serialize == "target_FAMETop.Top_clock_sink.ready", "clock packet handshake differs")
  println("PASS 52 internal queues: production SimWrapper endpoint multiplicity, all payload/handshake/host connections, Boolean clock-only wrapper ABI")
}
