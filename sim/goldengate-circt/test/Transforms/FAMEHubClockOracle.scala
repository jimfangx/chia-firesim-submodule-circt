// See LICENSE for license details.
// Execute the preserved Scala Golden Gate implementation for a two-clock hub.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
object FAMEHubClockOracle extends App {
  val input = """circuit Top :
  module Model :
    input bridge_clocks_0 : Clock
    input bridge_clocks_1 : Clock
    output out0 : UInt<1>
    output out1 : UInt<1>
    output targetClock0 : Clock
    output targetClock1 : Clock
    reg state0 : UInt<1>, bridge_clocks_0
    reg state1 : UInt<1>, bridge_clocks_1
    state0 <= not(state0)
    state1 <= not(state1)
    out0 <= state0
    out1 <= state1
    targetClock0 <= bridge_clocks_0
    targetClock1 <= bridge_clocks_1
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input bridge_clocks_0 : Clock
    input bridge_clocks_1 : Clock
    output out0 : UInt<1>
    output out1 : UInt<1>
    output targetClock0 : Clock
    output targetClock1 : Clock
    inst model of Model
    model.bridge_clocks_0 <= bridge_clocks_0
    model.bridge_clocks_1 <= bridge_clocks_1
    out0 <= model.out0
    out1 <= model.out1
    targetClock0 <= model.targetClock0
    targetClock1 <= model.targetClock1
"""
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  val top = ModuleTarget("Top", "Top")
  val model = ModuleTarget("Top", "Model")
  val clockNames = Seq("bridge_clocks_0", "bridge_clocks_1")
  val annos = Seq(
    FAMEHostClock(top.ref("hostClock")), FAMEHostReset(top.ref("hostReset")),
    FAMETransformAnnotation(model),
    FAMEChannelPortsAnnotation("bridge_clocks", None, clockNames.map(model.ref)),
    FAMEChannelConnectionAnnotation("bridge_clocks", TargetClockChannel(Seq(RationalClock("domain0", 1, 2), RationalClock("domain1", 1, 3)), Seq(2, 3)), None, None, Some(clockNames.map(top.ref)))
  ) ++ (0 until 2).flatMap { index =>
    val out = "out" + index
    val clk = "targetClock" + index
    Seq(
      FAMEChannelPortsAnnotation(out, Some(model.ref(clk)), Seq(model.ref(out))),
      FAMEChannelConnectionAnnotation(out, PipeChannel(0), Some(top.ref(clk)), Some(Seq(top.ref(out))), None)
    )
  }
  val result = new FAMETransform().execute(low.copy(annotations = annos))
  val writer = new java.io.PrintWriter(args(0))
  try writer.write(result.circuit.serialize) finally writer.close()
  def statements(stmt: Statement): Seq[Statement] = stmt match {
    case Block(stmts) => stmts.flatMap(statements)
    case other       => Seq(other)
  }
  val transformed = result.circuit.modules.collectFirst {
    case module: Module if module.name == "Model" => module
  }.get
  val body = statements(transformed.body)
  val connects = body.collect { case c: Connect => c.loc.serialize -> c.expr }.toMap
  val payload = transformed.ports.find(_.name == "bridge_clocks_sink").get.tpe
    .asInstanceOf[BundleType].fields.find(_.name == "bits").get.tpe.asInstanceOf[BundleType]
  require(payload.fields.map(_.name) == Seq("_0", "_1") &&
    payload.fields.forall(_.tpe == ClockType))
  for (i <- 0 until 2) {
    val register = body.collectFirst { case r: DefRegister if r.name == s"state$i" => r }.get
    require(register.clock.serialize == s"bridge_clocks_${i}_buffer.O")
    val enabled = body.collectFirst {
      case r: DefRegister if r.name == s"bridge_clocks_${i}_enabled" => r
    }.get
    require(enabled.clock.serialize == "hostClock" && enabled.reset.serialize == "hostReset" &&
      enabled.init.asInstanceOf[UIntLiteral].value == 0)
    require(connects(s"bridge_clocks_${i}_buffer.I").serialize == "hostClock")
  }
  def evaluate(expr: Expression, values: Map[String, Int]): Int = expr match {
    case Mux(sel, high, low, _) => evaluate(if (evaluate(sel, values) != 0) high else low, values)
    case DoPrim(PrimOps.AsUInt, Seq(input), _, _) => evaluate(input, values)
    case DoPrim(PrimOps.Not, Seq(input), _, _) => 1 - evaluate(input, values)
    case DoPrim(PrimOps.And, Seq(left, right), _, _) => evaluate(left, values) & evaluate(right, values)
    case other => values(other.serialize)
  }
  // Evaluate the actual emitted expressions, including cross-domain cases.
  for (mask <- 0 until 64) {
    val values = Map(
      "bridge_clocks_sink.bits._0" -> (mask & 1),
      "bridge_clocks_sink.bits._1" -> ((mask >> 1) & 1),
      "bridge_clocks_0_enabled" -> ((mask >> 2) & 1),
      "bridge_clocks_1_enabled" -> ((mask >> 3) & 1),
      "targetCycleFinishing" -> ((mask >> 4) & 1),
      "hostReset" -> ((mask >> 5) & 1))
    val outputs = Seq("bridge_clocks_0_enabled", "bridge_clocks_1_enabled",
      "bridge_clocks_0_buffer.CE", "bridge_clocks_1_buffer.CE")
      .map(name => evaluate(connects(name), values))
    println(s"HUB $mask ${outputs.mkString(" ")}")
  }
  result.annotations.collect { case anno: midas.InternalXDCAnnotation =>
    println("XDC " + anno.toString)
  }
}
