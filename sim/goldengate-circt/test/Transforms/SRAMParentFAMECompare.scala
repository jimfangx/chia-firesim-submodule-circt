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
  require(expectedABI.forall { case (name, value) => actualABI.get(name).contains(value) }, "common Rocket port ABI differs")
  val extraPorts = actualABI.keySet -- expectedABI.keySet
  val knownAliases = Set("io_imem_bht_update_bits_pc", "io_ptw_sfence_valid", "io_ptw_sfence_bits_rs1",
    "io_ptw_sfence_bits_rs2", "io_ptw_sfence_bits_addr", "io_ptw_sfence_bits_asid",
    "io_ptw_sfence_bits_hv", "io_ptw_sfence_bits_hg").map(_ + "_source")
  require(extraPorts == knownAliases, s"unexpected parent ABI mismatch: $extraPorts")
  println(s"PASS ${expectedABI.size} common parent ports; REMAINING ${extraPorts.size} duplicate output-alias channels")

  // Expand the emitted nodes before comparing completion. The only accepted
  // additional terms correspond to the eight independently observed alias ports.
  // Every common SFC condition, including clock valid, must remain identical.
  val actualHW = Hardware(module(native, "Rocket"))
  val expectedHW = Hardware(module(sfc, "Rocket"))
  def completionTerms(h: Hardware, e: Expression): Seq[String] = e match {
    case r: WRef if h.nodes.contains(r.name) => completionTerms(h, h.nodes(r.name))
    case DoPrim(PrimOps.And, Seq(a, b), _, _) => completionTerms(h, a) ++ completionTerms(h, b)
    case other => Seq(h.canonical(other))
  }
  val aliasConditions = knownAliases.map { port =>
    val reg = port.stripSuffix("_source") + "_fired_0"
    s"or(and($port.ready,$port.valid),$reg)"
  }
  for (key <- Seq("targetCycleFinishing", "clock_sink.ready")) {
    val actual = completionTerms(actualHW, actualHW.connects(key))
    val expected = completionTerms(expectedHW, expectedHW.connects(key))
    require(actual.diff(expected).toSet == aliasConditions && expected.diff(actual).isEmpty &&
      actual.size == expected.size + aliasConditions.size, s"$key completion conditions differ beyond known aliases")
    println(s"PASS $key: ${expected.size} SFC conditions after expanding native nodes; eight known alias conditions remain")
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
  require(matches.size == 33, s"unexpected path transfer match count: ${matches.size}")
  val missing = expectedPaths.diff(actualPaths); val extra = actualPaths.diff(expectedPaths)
  require(extra.forall(a => compact(render(a)).contains(">io_hartid\"") ||
    compact(render(a)).contains(">io_dmem_resp_bits_data\"")), "unexpected metadata mismatch beyond unchannelized passthroughs")
  println("PASS 33/39 CombinationalPath records: actual model bindings, ordered sources and payload targets; REMAINING six top passthrough records")
  for ((s, n) <- missing.zip(extra)) println(s"REMAINING path: SFC=${compact(render(s))}; native=${compact(render(n))}")
}
