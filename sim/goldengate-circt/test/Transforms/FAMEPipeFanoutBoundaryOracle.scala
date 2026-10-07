// See LICENSE for license details.
// Elaborate the production SimWrapper for mixed bridge/model and internal
// target-sourced fanout. TargetBox stays an extmodule: this oracle checks the
// actual wrapper interface and branch wiring; the queue oracle checks behavior.
import firrtl._
import firrtl.ir._
import firrtl.annotations.ReferenceTarget
import midas.core.{SimWrapper, SimWrapperConfig}
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
import org.chipsalliance.cde.config.Parameters

object FAMEPipeFanoutBoundaryOracle extends App {
  implicit val p: Parameters = Parameters.empty
  val destination = new java.io.File(args(0)); destination.mkdirs()
  def target(name: String) = ReferenceTarget("Top", "Top", Nil, name, Nil).field("bits")
  val producer = target("producer_source")
  val sinks = Seq(target("model1_sink"), target("model2_sink"))
  val clock = target("ticks")
  def channelType(payload: Type) = BundleType(Seq(
    Field("ready", Flip, UIntType(IntWidth(1))), Field("valid", Default, UIntType(IntWidth(1))),
    Field("bits", Default, payload)))
  val leafTypes = (Seq(producer) ++ sinks).map(rt =>
    rt.copy(component = Nil) -> Port(NoInfo, rt.ref, Output, channelType(UIntType(IntWidth(16))))).toMap +
    (clock.copy(component = Nil) -> Port(NoInfo, clock.ref, Input, channelType(ClockType)))
  val branches = (0 until 3).map(i => FAMEChannelConnectionAnnotation(
    s"fork$i", PipeChannel(if (i == 1) 1 else 0), None, Some(Seq(producer)),
    if (i == 0) None else Some(Seq(sinks(i-1)))))
  val clockChannel = FAMEChannelConnectionAnnotation("ticks",
    TargetClockChannel(Seq(RationalClock("clock", 1, 1)), Seq(1)), None, None, Some(Seq(clock)))
  for ((name, channels) <- Seq("mixed" -> branches, "internal-fanout" -> branches.tail,
                               "internal-single" -> branches.slice(1, 2))) {
    val fanout = if (channels.size > 1)
      Seq(FAMEChannelFanoutAnnotation(channels.map(_.globalName).reverse)) else Nil
    val config = SimWrapperConfig(channels ++ fanout :+ clockChannel, leafTypes)
    val input = (new chisel3.stage.ChiselStage).emitChirrtl(new SimWrapper(config),
      Array("--target-dir", destination.getAbsolutePath))
    val circuit = new LowFirrtlCompiler()
      .compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil).circuit
    val wrapper = circuit.modules.collectFirst { case m: Module if m.name == circuit.main => m }.get
    val externalData = wrapper.ports.filter(_.name.startsWith("channelPorts_producer_source"))
    require(externalData.size == (if (name == "mixed") 3 else 0), s"$name external data ports")
    require(!wrapper.ports.exists(_.name.contains("model1_sink")) &&
            !wrapper.ports.exists(_.name.contains("model2_sink")), s"$name exposed model sink")
    val writer = new java.io.PrintWriter(new java.io.File(destination, s"$name.sfc.fir"))
    try writer.write(circuit.serialize) finally writer.close()
    println(s"PASS $name: ${channels.size} queues; ${externalData.map(_.name).mkString(",")}")
  }
}
