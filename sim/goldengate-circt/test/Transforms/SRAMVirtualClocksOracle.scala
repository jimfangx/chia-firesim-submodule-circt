// See LICENSE for license details.
// Build only the independent SFC oracle; parse the native output for comparison.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import scala.collection.mutable

object SRAMVirtualClocksOracle extends App {
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
  for ((name, ram, expectedInstances) <- Seq(
      ("fanout", "ram", 4), ("golden-rocket", "rf", 1))) {
    val annotations = JsonProtocol.deserialize(read(s"oracle/$name.channels.sfc.json"))
    // Parsing loses reference types/kinds. Midas resolves them before FAME;
    // its clock substitution compares typed references to the original ports.
    val state = new ResolveAndCheck().runTransform(CircuitState(
      Parser.parse(read(s"oracle/$name.channels.sfc.fir")), LowForm, annotations))
    val original = state.circuit
    val analysis = new FAMEChannelAnalysis(state)
    val before = original.modules.find(_.name == ram).get.asInstanceOf[Module]
    val added = mutable.ArrayBuffer[Annotation]()
    val oracle = FAMEModuleTransformer(before, analysis, added)
    write(s"oracle/$name.virtual-clock.sfc.fir", oracle.serialize)
    write(s"oracle/$name.virtual-clock.sfc.json", JsonProtocol.serialize(added.toSeq))
    require(added.isEmpty, "non-hub virtual clock must not emit generated-clock XDC")
    val candidate = Parser.parse(read(s"$name-clocks/post-sram-clocks.fir")
      .replaceAll("(?m)^(\\s*)public module ", "$1module "))
    val native = candidate.modules.find(_.name == ram).get.asInstanceOf[Module]
    val targetClock = before.ports.filter(_.tpe == ClockType).map(_.name)
    require(targetClock.size == 1)
    val clock = targetClock.head
    val enabled = s"${clock}_enabled"
    val buffer = s"${clock}_buffer"
    val finishing = "targetCycleFinishing"
    def clocks(module: Module) = module.ports.filter(_.tpe == ClockType).map(_.name)
    require(clocks(oracle) == Seq("hostClock") && clocks(native) == clocks(oracle))
    def portABI(ports: Seq[Port]) = ports.map(p => (p.name, p.direction, p.tpe.serialize))
    require(portABI(native.ports) == portABI(Seq(
      Port(NoInfo, "hostClock", Input, ClockType),
      Port(NoInfo, "hostReset", Input, UIntType(IntWidth(1)))) ++
      before.ports.filterNot(_.name == clock)), "native scalar data ABI changed")

    def clockSemantics(module: Module) = {
      val stmts = statements(module)
      val regs = stmts.collect { case r: DefRegister if r.name == enabled => r }
      require(regs.size == 1, s"${module.name}: virtual clock enable must be unique")
      val reg = regs.head
      require(reg.clock.serialize == "hostClock" && reg.reset.serialize == "hostReset")
      require(reg.init.asInstanceOf[UIntLiteral].value == 0)
      val gates = stmts.collect {
        case i: WDefInstance if i.module == "AbstractClockGate" => i
      }
      require(gates.size == 1 && gates.head.name == buffer)
      val connects = stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs }.toMap
      require(connects(s"$buffer.I").serialize == "hostClock")
      val nodes = stmts.collect { case DefNode(_, n, value) => n -> value }.toMap
      def evaluate(expression: Expression, values: Map[String, BigInt]): BigInt = expression match {
        case UIntLiteral(value, _) => value
        case reference: WRef => values.getOrElse(reference.name,
          evaluate(nodes.getOrElse(reference.name,
            throw new Exception(s"unknown boolean leaf: ${reference.serialize}")), values))
        case Mux(condition, t, f, _) =>
          evaluate(if (evaluate(condition, values) != 0) t else f, values)
        case DoPrim(PrimOps.AsUInt, Seq(value), _, _) => evaluate(value, values)
        case DoPrim(PrimOps.And, Seq(a, b), _, _) => evaluate(a, values) & evaluate(b, values)
        case DoPrim(PrimOps.Not, Seq(a), _, _) => BigInt(1) ^ evaluate(a, values)
        case other => throw new Exception(s"unexpected clock expression: ${other.serialize}")
      }
      val truth = for (f <- 0 to 1; e <- 0 to 1; r <- 0 to 1) yield {
        val values = Map(finishing -> BigInt(f), enabled -> BigInt(e), "hostReset" -> BigInt(r))
        val next = evaluate(connects(enabled), values)
        val gate = evaluate(connects(s"$buffer.CE"), values)
        require(next == (if (f == 1) BigInt(1) else BigInt(e)))
        require(gate == BigInt(e & f & (1 ^ r)))
        (f, e, r, next, gate)
      }
      val memories = stmts.collect { case memory: DefMemory => memory }
      val memoryClocks = memories.flatMap { memory =>
        (memory.readers ++ memory.writers ++ memory.readwriters).map { port =>
          val sink = s"${memory.name}.$port.clk"
          require(connects(sink).serialize == s"$buffer.O", s"ungated SRAM clock: $sink")
          sink
        }
      }
      (reg.tpe.serialize, memories.map(m => (m.name, m.dataType.serialize, m.depth,
        m.readLatency, m.writeLatency, m.readers, m.writers, m.readwriters, m.readUnderWrite)),
        truth, memoryClocks)
    }
    require(clockSemantics(native) == clockSemantics(oracle), "virtual clock hardware differs from SFC")
    val top = candidate.modules.find(_.name == candidate.main).get.asInstanceOf[Module]
    val topStatements = statements(top)
    val instances = topStatements.collect { case i: WDefInstance if i.module == ram => i }
    require(instances.size == expectedInstances)
    val assignments = topStatements.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs.serialize }.toMap
    instances.foreach { instance =>
      require(assignments(s"${instance.name}.hostClock") == "hostClock")
      require(assignments(s"${instance.name}.hostReset") == "hostReset")
      require(!assignments.contains(s"${instance.name}.$clock"), "stale instance clock connect")
    }
    println(s"PASS $name: SFC virtual-clock reset/next-state/gate truth tables, gated memory ports, " +
      s"scalar data ABI and $expectedInstances promoted host interfaces match")
  }
}
