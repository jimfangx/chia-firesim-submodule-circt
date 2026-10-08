// See LICENSE for license details.
// Differentially evaluates exported native operations, not a second copy of
// the timing algorithm. SFC alone elaborates the semantic oracle.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations.JsonProtocol
import scala.collection.mutable

object AsyncRAMModelFiles {
  def read(file: java.io.File): String = {
    val source = scala.io.Source.fromFile(file)
    try source.mkString finally source.close()
  }
  def write(file: java.io.File, text: String): Unit = {
    val out = new java.io.PrintWriter(file)
    try out.write(text) finally out.close()
  }
  def selected(file: java.io.File, name: String = "RamModel"): Module = {
    val lines = read(file).replaceAll("(?m)^(\\s*)public module ", "$1module ").linesIterator.toSeq
    val start = lines.indexWhere(_.matches(s"  module $name :.*"))
    require(start >= 0, s"$file has no $name")
    val body = lines.drop(start).takeWhile(line => line == lines(start) ||
      !line.matches("  (module|extmodule) .*"))
    Parser.parse(s"circuit $name :\n" + body.mkString("\n") + "\n").modules.head.asInstanceOf[Module]
  }
}
object AsyncRAMModelOracle extends App {
  import AsyncRAMModelFiles._
  val output = new java.io.File(args(0)); output.mkdirs()
  val previous = new java.io.File(args(1))
  // Pinned CIRCT exporter writes a public keyword even at FIRRTL 1.2. The
  // parser rejects that version/keyword pair; normalize syntax only.
  write(new java.io.File(output, "combined.input.fir"), read(new java.io.File(previous,
    "golden-rocket-native/post-ram-adapter.fir")).replaceAll("(?m)^(\\s*)public module ", "$1module "))
  write(new java.io.File(output, "combined.input.json"), read(new java.io.File(previous,
    "golden-rocket-native/post-ram-adapter-all.json")))
  for (name <- Seq("golden-rocket", "aggregate", "multiport")) {
    val expected = if (name != "multiport") selected(new java.io.File(previous, s"$name.expected.fir")) else {
      val chirrtl = chisel3.stage.ChiselStage.convert(new midas.models.sram.AsyncMemChiselModel(16, 23, 3, 3))
      new MiddleFirrtlCompiler().compile(CircuitState(chirrtl, ChirrtlForm, Nil), Nil)
        .circuit.modules.head.asInstanceOf[Module].copy(name = "RamModel")
    }
    val circuit = Circuit(NoInfo, Seq(expected), "RamModel")
    write(new java.io.File(output, s"$name.expected.fir"), circuit.serialize)
    // The native emitter consumes a declaration only; no Chisel/SFC body.
    write(new java.io.File(output, s"$name.input.fir"),
      circuit.copy(modules = Seq(expected.copy(body = EmptyStmt))).serialize)
    write(new java.io.File(output, s"$name.input.json"), "[]")
  }
}
object AsyncRAMModelCompare extends App {
  import AsyncRAMModelFiles._
  val directory = new java.io.File(args(0))
  def statements(s: Statement): Seq[Statement] = {
    val out = mutable.ArrayBuffer[Statement]()
    def visit(v: Statement): Unit = { out += v; v.foreachStmt(visit) }
    visit(s); out.toSeq
  }
  // Ground leaf widths and directions are obtained from each artifact's ABI.
  def leaves(name: String, t: Type, input: Boolean): Seq[(String, Int, Boolean)] = t match {
    case UIntType(IntWidth(w)) => Seq((name, w.toInt, input))
    case ClockType => Seq((name, 1, input))
    case VectorType(element, size) => (0 until size).flatMap(i => leaves(s"$name[$i]", element, input))
    case BundleType(fields) => fields.flatMap(f => leaves(s"$name.${f.name}", f.tpe,
      if (f.flip == Flip) !input else input))
    case other => sys.error(s"unsupported leaf $name: $other")
  }
  class Machine(val module: Module) {
    val stmts = statements(module.body)
    require(stmts.forall(s => s.isInstanceOf[Block] || s.isInstanceOf[DefNode] ||
      s.isInstanceOf[DefWire] || s.isInstanceOf[DefRegister] || s.isInstanceOf[DefMemory] ||
      s.isInstanceOf[Connect] || s == EmptyStmt), "unexpected timing-model operation")
    val nodes = stmts.collect { case DefNode(_, name, value) => name -> value }.toMap
    val connects = stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs }.toMap
    val registers = stmts.collect { case r: DefRegister => r }
    require(registers.forall(r => r.clock.serialize == "clock" && r.reset == UIntLiteral(0, IntWidth(1))),
      "unexpected clock or implicit register reset")
    val regs = registers.flatMap(r => leaves(r.name, r.tpe, false)).map { case (n, w, _) => n -> w }.toMap
    val abi = module.ports.flatMap(p => leaves(p.name, p.tpe, p.direction == Input)).map(v => v._1 -> v).toMap
    val inputs = abi.values.filter(_._3).map { case (n, w, _) => n -> w }.toMap
    val outputs = abi.values.filterNot(_._3).map(_._1).toSeq.sorted
    val memories = stmts.collect { case m: DefMemory => m }
    require(memories.size == 1 && memories.head.readers == Seq("read_data_async") &&
      memories.head.writers == Seq("MPORT") && memories.head.readwriters.isEmpty &&
      memories.head.readLatency == 0 && memories.head.writeLatency == 1 &&
      memories.head.readUnderWrite == ReadUnderWrite.Undefined, "unexpected RAM contract")
    val memory = memories.head
    val dataWidth = memory.dataType.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width.toInt
    def evaluate(input: Map[String, BigInt], state: Map[String, BigInt], data: Vector[BigInt]):
        (Map[String, BigInt], Map[String, BigInt], Option[(Int, BigInt)], Int) = {
      val cache = mutable.Map[String, BigInt]()
      def ref(name: String): BigInt = input.getOrElse(name, state.getOrElse(name,
        cache.getOrElseUpdate(name, if (name == "data.read_data_async.data")
          data(ref("data.read_data_async.addr").toInt)
        else eval(nodes.getOrElse(name, connects.getOrElse(name, sys.error(s"undriven $name")))))))
      def eval(e: Expression): BigInt = e match {
        case UIntLiteral(value, _) => value
        case Mux(c, yes, no, _) => eval(if (eval(c) != 0) yes else no)
        // SFC validif marks unused write fields; compare them only when en.
        case ValidIf(_, value, _) => eval(value)
        case DoPrim(op, operands, consts, _) =>
          val vs = operands.map(eval)
          op match {
            case PrimOps.And => vs(0) & vs(1)
            case PrimOps.Or => vs(0) | vs(1)
            case PrimOps.Not => require(vs(0) <= 1); BigInt(1) ^ vs(0)
            case PrimOps.Eq => if (vs(0) == vs(1)) BigInt(1) else BigInt(0)
            case PrimOps.Neq => if (vs(0) != vs(1)) BigInt(1) else BigInt(0)
            case PrimOps.Bits => (vs.head >> consts(1).toInt) & ((BigInt(1) << (consts(0) - consts(1) + 1).toInt) - 1)
            case PrimOps.Pad => vs.head
            case other => sys.error(s"unsupported primitive $other")
          }
        case other => ref(other.serialize)
      }
      val next = regs.map { case (name, width) =>
        name -> (eval(connects(name)) & ((BigInt(1) << width) - 1)) }
      val values = outputs.map(n => n -> ref(n)).toMap
      require(ref("data.read_data_async.clk") == 1 && ref("data.read_data_async.en") == 1,
        "reader clock/enable differ")
      val write = if ((ref("data.MPORT.en") & ref("data.MPORT.mask")) != 0) {
        require(ref("data.MPORT.clk") == 1, "writer clock differs")
        Some(ref("data.MPORT.addr").toInt -> ref("data.MPORT.data"))
      } else None
      (values, next, write, ref("data.read_data_async.addr").toInt)
    }
  }
  for (name <- Seq("golden-rocket", "aggregate", "multiport")) {
    val reference = new Machine(selected(new java.io.File(directory, s"$name.expected.fir")))
    val native = new Machine(selected(new java.io.File(directory, s"$name-native/post-async-ram.fir")))
    require(reference.abi == native.abi && reference.regs == native.regs, s"$name ABI/registers differ")
    require(reference.memory.copy(info = NoInfo) == native.memory.copy(info = NoInfo), s"$name memory differs")
    require(read(new java.io.File(directory, s"$name-native/post-async-ram-all.json")).trim == "[]",
      "emitter changed annotations")
    val random = new scala.util.Random(0x55L)
    def values(widths: Map[String, Int]) = widths.map { case (n, w) => n -> BigInt(w, random) }
    var state = values(reference.regs)
    var data = Vector.fill(reference.memory.depth.toInt)(BigInt(reference.dataWidth, random))
    val coverage = mutable.Map[String, Int]().withDefaultValue(0)
    // Arbitrary complete states exercise every state encoding and combinations
    // including simultaneous final response/write, independent of reachability.
    // Stateful cycles then check sampled data and stalled response retention.
    for (cycle <- 0 until 20000) {
      if (cycle < 10000) state = values(reference.regs)
      val input = values(reference.inputs) ++ Map("clock" -> BigInt(1),
        "reset" -> BigInt(if (cycle == 10000 || random.nextInt(64) == 0) 1 else 0))
      val expected = reference.evaluate(input, state, data)
      val actual = native.evaluate(input, state, data)
      require(actual == expected, s"$name cycle $cycle differs: $actual != $expected; $state; $input")
      if (expected._3.nonEmpty) coverage("write") += 1
      if (input("reset") == 1) coverage("host-reset") += 1
      if (input("channels.reset.bits") == 1) coverage("target-reset") += 1
      for ((n, v) <- state if n.startsWith("read_state[")) coverage(s"state-$v") += 1
      if (state.exists { case (n, v) => n.startsWith("read_state[") && v == 2 }) coverage("buffered") += 1
      if (cycle >= 10000) {
        state = expected._2
        expected._3.foreach { case (addr, value) => data = data.updated(addr, value) }
      }
    }
    for (key <- Seq("write", "host-reset", "target-reset", "state-0", "state-1", "state-2", "state-3", "buffered"))
      require(coverage(key) > 0, s"$name missing $key coverage")
    println(s"PASS $name: 20000 complete transitions, ABI, storage, outputs, every register D, read address and enabled writes; $coverage")
  }
}

// The combined probe starts from the native adapter boundary, retaining the
// complete Rocket hierarchy and annotation stream rather than a stub circuit.
object AsyncRAMModelCombinedCompare extends App {
  import AsyncRAMModelFiles._
  val previous = new java.io.File(args(0)); val output = new java.io.File(args(1))
  val before = selected(new java.io.File(previous, "golden-rocket-native/post-ram-adapter.fir"), "rf")
  val after = selected(new java.io.File(output, "golden-rocket-combined/post-async-ram.fir"), "rf")
  require(before.ports == after.ports, "combined emission changed wrapper ports")
  def equations(s: Statement): Seq[(String, String)] = {
    val out = mutable.ArrayBuffer[(String, String)]()
    def visit(v: Statement): Unit = {
      v match { case Connect(_, lhs, rhs) => out += lhs.serialize -> rhs.serialize; case _ => }
      v.foreachStmt(visit)
    }
    visit(s); out.toSeq.sorted
  }
  require(equations(before.body) == equations(after.body), "combined emission changed wrapper equations")
  val input = JsonProtocol.deserialize(read(new java.io.File(previous,
    "golden-rocket-native/post-ram-adapter-all.json")))
  val actual = JsonProtocol.deserialize(read(new java.io.File(output,
    "golden-rocket-combined/post-async-ram-all.json")))
  require(input == actual, "combined emission changed annotations")
  val expected = selected(new java.io.File(output, "golden-rocket-native/post-async-ram.fir"))
  val combined = selected(new java.io.File(output, "golden-rocket-combined/post-async-ram.fir"))
  require(equations(expected.body) == equations(combined.body) &&
    expected.ports.map(_.copy(info = NoInfo)) == combined.ports.map(_.copy(info = NoInfo)),
    "combined host differs from independently checked native host")
  println(s"PASS combined golden Rocket: ${input.size} retained annotations and ${equations(before.body).size} unchanged wrapper equations")
}
