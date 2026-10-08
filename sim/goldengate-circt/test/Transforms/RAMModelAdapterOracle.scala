// See LICENSE for license details.
// SFC generates the reference wrapper and the shared Chisel timing module.
// The candidate wrapper comes from native FAME and is never compiled by SFC.
package midas.passes.fame

import firrtl._
import firrtl.ir._
import firrtl.annotations._
import scala.collection.mutable

object RAMModelAdapterOracle extends App {
  val output = new java.io.File(args(0)); output.mkdirs()
  val previous = new java.io.File(args(1))
  def read(file: java.io.File) = {
    val source = scala.io.Source.fromFile(file)
    try source.mkString finally source.close()
  }
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(output, name))
    try out.write(text) finally out.close()
  }
  def parseCandidate(text: String) = Parser.parse(text.replaceAll("(?m)^(\\s*)public module ", "$1module "))
  def emit(name: String, input: CircuitState, candidate: Circuit): Unit = {
    val reference = new EmitAndWrapRAMModels().runTransform(input)
    // Normalize only the shared Chisel constructor: this CIRCT parser does
    // not accept legacy validif. The native wrapper is not passed through SFC.
    val host = firrtl.passes.RemoveValidIf.runTransform(reference)
    val added = host.circuit.modules.filterNot(m => input.circuit.modules.exists(_.name == m.name))
    require(added.size == 1, "probe must select one RAM definition")
    write(s"$name.expected.fir", reference.circuit.serialize)
    write(s"$name.input.fir", candidate.copy(modules = candidate.modules ++ added).serialize)
    write(s"$name.input.json", JsonProtocol.serialize(input.annotations))
    println(s"Prepared $name native wrapper input with shared Chisel timing module ${added.head.name}")
  }
  for ((name, module) <- Seq("golden-rocket" -> "rf")) {
    val prepared = CircuitState(Parser.parse(read(new java.io.File(previous, s"oracle/$name.channels.sfc.fir"))),
      LowForm, JsonProtocol.deserialize(read(new java.io.File(previous, s"oracle/$name.channels.sfc.json"))))
    val transformed = AddRemainingFanoutAnnotations.runTransform(new FAMETransform().runTransform(
      new ResolveAndCheck().runTransform(prepared)))
    val fame = transformed.copy(circuit = transformed.circuit.copy(
      modules = transformed.circuit.modules :+ midas.passes.DefineAbstractClockGate.blackbox))
    val native = parseCandidate(read(new java.io.File(previous,
      s"${if (name == "fanout") "fanout-parent-fame" else "golden-rocket-parent-fame"}/post-sram-parent-fame.fir")))
    // Only the selected wrapper is under test here; SFC and native hub names
    // can differ. Keep the candidate wrapper and the independently prepared
    // reference hierarchy, without running a Scala transform on the candidate.
    val selected = native.modules.find(_.name == module).get
    emit(name, fame, fame.circuit.copy(modules = fame.circuit.modules.map(m => if (m.name == module) selected else m)))
  }
  // Multiple fields in one payload still have distinct annotation identities.
  // In particular SFC includes the other field's valid in each ready equation.
  val u1 = UIntType(IntWidth(1)); val addr = UIntType(IntWidth(3)); val data = UIntType(IntWidth(17))
  def payload(fields: (String, Type)*) = BundleType(fields.map { case (n, t) => Field(n, Default, t) })
  val ports = Seq(Port(NoInfo, "hostClock", Input, ClockType), Port(NoInfo, "hostReset", Input, u1),
    Port(NoInfo, "read", Input, Decouple(payload("addr" -> addr, "en" -> u1))),
    Port(NoInfo, "response", Output, Decouple(data)),
    Port(NoInfo, "write", Input, Decouple(payload("addr" -> addr, "en" -> u1, "data" -> data, "mask" -> u1))),
    Port(NoInfo, "write2", Input, Decouple(payload("addr" -> addr, "en" -> u1, "data" -> data, "mask" -> u1))))
  val module = Module(NoInfo, "Aggregate", ports, EmptyStmt)
  val mt = ModuleTarget("Aggregate", "Aggregate")
  val annos = Seq(ModelReadPort(addr = mt.ref("read").field("bits").field("addr"),
    en = mt.ref("read").field("bits").field("en"), data = mt.ref("response").field("bits")),
    ModelWritePort(addr = mt.ref("write").field("bits").field("addr"),
      en = mt.ref("write").field("bits").field("en"), data = mt.ref("write").field("bits").field("data"),
      mask = mt.ref("write").field("bits").field("mask")),
    ModelWritePort(addr = mt.ref("write2").field("bits").field("addr"),
      en = mt.ref("write2").field("bits").field("en"), data = mt.ref("write2").field("bits").field("data"),
      mask = mt.ref("write2").field("bits").field("mask")))
  val aggregate = CircuitState(Circuit(NoInfo, Seq(module), "Aggregate"), LowForm, annos ++ annos)
  emit("aggregate", aggregate, aggregate.circuit)
}

object RAMModelAdapterCompare extends App {
  val directory = new java.io.File(args(0))
  def read(path: String) = {
    val source = scala.io.Source.fromFile(new java.io.File(directory, path))
    try source.mkString finally source.close()
  }
  // Parse the actual selected module, excluding unrelated hub expressions
  // which CIRCT may wrap across lines beyond the pinned parser's grammar.
  def selected(path: String, name: String): Module = {
    val lines = read(path).replaceAll("(?m)^(\\s*)public module ", "$1module ").linesIterator.toSeq
    val start = lines.indexWhere(_.matches(s"  module $name :.*"))
    require(start >= 0, s"$path has no selected wrapper $name")
    val body = lines.drop(start).takeWhile(line => line == lines(start) ||
      !line.matches("  (module|extmodule) .*"))
    Parser.parse(s"circuit $name :\n" + body.mkString("\n") + "\n").modules.head.asInstanceOf[Module]
  }
  def statements(s: Statement): Seq[Statement] = {
    val result = mutable.ArrayBuffer[Statement]()
    def visit(s: Statement): Unit = { result += s; s.foreachStmt(visit) }
    visit(s); result.toSeq
  }
  for ((name, module) <- Seq("golden-rocket" -> "rf", "aggregate" -> "Aggregate")) {
    val expected = selected(s"$name.expected.fir", module)
    val native = selected(s"$name-native/post-ram-adapter.fir", module)
    def abi(m: Module) = m.ports.map(p => p.name -> (p.direction, p.tpe.serialize.replace(" ", ""))).toMap
    require(abi(expected) == abi(native), s"$name wrapper port ABI differs")
    require(selected(s"$name.input.fir", module).ports.map(p => p.copy(info = NoInfo)) ==
      native.ports.map(p => p.copy(info = NoInfo)), s"$name adapter changed its input port order")
    def equations(m: Module): Map[String, String] = {
      val stmts = statements(m.body)
      val nodes = stmts.collect { case DefNode(_, name, value) => name -> value }.toMap
      val connects = stmts.collect { case Connect(_, lhs, rhs) => lhs.serialize -> rhs }.toMap
      // SFC's mutable annotation set does not specify vector order. Compare
      // every command/response identity using its bound address input.
      val aliases = connects.collect {
        case (lhs, rhs) if lhs.matches("model.channels.(read|write)_cmds\\[\\d+\\].bits.addr") =>
          lhs.stripSuffix(".bits.addr") -> s"command[${rhs.serialize}]"
      } ++ connects.collect {
        case (lhs, rhs) if lhs.matches("model.channels.read_cmds\\[\\d+\\].bits.addr") =>
          lhs.stripSuffix(".bits.addr").replace("read_cmds", "read_resps") -> s"response[${rhs.serialize}]"
      }
      def identity(s: String) = aliases.foldLeft(s) { case (text, (from, to)) => text.replace(from, to) }
      def canonical(e: Expression): String = e match {
        case UIntLiteral(value, _) => value.toString
        case r: WRef if nodes.contains(r.name) => canonical(nodes(r.name))
        case DoPrim(PrimOps.And, Seq(a, b), _, _) =>
          def terms(v: Expression): Seq[String] = v match {
            case r: WRef if nodes.contains(r.name) => terms(nodes(r.name))
            case DoPrim(PrimOps.And, Seq(x, y), _, _) => terms(x) ++ terms(y)
            case other => Seq(canonical(other))
          }
          val vs = (terms(a) ++ terms(b)).distinct.sorted.filterNot(_ == "1")
          if (vs.isEmpty) "1" else if (vs.size == 1) vs.head else s"and(${vs.mkString(",")})"
        case other => identity(other.serialize)
      }
      require(!stmts.exists(s => s.isInstanceOf[DefMemory] || s.isInstanceOf[DefRegister]),
        s"$name wrapper retained target storage or FAME state")
      connects.map { case (lhs, rhs) => identity(lhs) -> canonical(rhs) }
    }
    val actual = equations(native); val reference = equations(expected)
    for ((key, value) <- reference)
      require(actual.get(key).contains(value), s"$name/$key: ${actual.get(key)} != $value")
    require(actual.keySet == reference.keySet, s"$name unexpected adapter connections")
    val inputAnnotations = JsonProtocol.deserialize(read(s"$name.input.json"))
    val nativeAnnotations = JsonProtocol.deserialize(read(s"$name-native/post-ram-adapter-all.json"))
    require(nativeAnnotations == inputAnnotations, s"$name retained annotations changed")
    println(s"PASS $name: ${reference.size} command/response/ready/valid/host/reset-token equations, port ABI and all retained annotations")
  }
}
