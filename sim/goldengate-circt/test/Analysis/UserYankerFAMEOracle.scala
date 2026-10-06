// See LICENSE for license details.
// Run against the extracted immutable UserYanker boundary (see WhenDependencies.md).
// This calls the preserved SFC connectivity and Golden Gate valid-rule implementations.
import firrtl._
import firrtl.ir._
import midas.passes.fame.{FAME1InputChannel, FAME1OutputChannel}
object Oracle extends App {
  val c = Parser.parse(scala.io.Source.fromFile(args(0)).mkString)
  val low = new LowFirrtlCompiler().compile(CircuitState(c, ChirrtlForm), Nil)
  val m = low.circuit.modules.collectFirst { case x: Module if x.name == "UserYankerProbe" => x }.get
  val deps = new firrtl.transforms.CheckCombLoops().analyze(low)(m.name).getEdgeMap
  Seq("size", "source").foreach { o => println("sfc " + o + " <- " + deps(o).mkString(", ")) }
  def group(port: String): String = {
    if (port.startsWith("size")) "sizeInput"
    else if (port.startsWith("source")) "sourceInput"
    else if (port == "index") "indexInput"
    else sys.error("unexpected extracted input " + port)
  }
  val bit = UIntType(IntWidth(1))
  def fired(name: String) = DefRegister(NoInfo, name + "_fired_0", bit, WRef("hostClock"), WRef("hostReset"), UIntLiteral(0))
  val inputs = Seq("indexInput", "sourceInput", "sizeInput").map { n =>
    n -> FAME1InputChannel(n, UIntLiteral(1), m.ports.filter(p => p.direction == Input && group(p.name) == n), fired(n))
  }.toMap
  Seq("source" -> Seq("source"), "pair" -> Seq("size", "source")).foreach { case (name, ports) =>
    val required = ports.flatMap(p => deps(p).map(group)).distinct
    val out = FAME1OutputChannel(name, UIntLiteral(1), m.ports.filter(p => ports.contains(p.name)), fired(name))
    val expr = out.setValid(WRef("targetCycleFinishing"), required.map(inputs)).asInstanceOf[Connect].expr
    def eval(e: Expression, flags: Int): Boolean = e match {
      case WSubField(WRef(n, _, _, _), "valid", _, _) =>
        (flags & (1 << Map("indexInput_sink" -> 0, "sourceInput_sink" -> 1,
                           "sizeInput_sink" -> 2)(n))) != 0
      case WRef(n, _, _, _) =>
        (flags & (1 << Map("source_fired_0" -> 3, "pair_fired_0" -> 4)(n))) != 0
      case DoPrim(PrimOps.And, a, _, _) => eval(a(0), flags) && eval(a(1), flags)
      case DoPrim(PrimOps.Not, a, _, _) => !eval(a(0), flags)
      case _ => sys.error("unsupported oracle expression " + e.serialize)
    }
    println("valid " + name + " <- " + (0 until 32).map(f => if (eval(expr, f)) '1' else '0').mkString)
    println("equation " + name + " <- " + expr.serialize)
  }
}
