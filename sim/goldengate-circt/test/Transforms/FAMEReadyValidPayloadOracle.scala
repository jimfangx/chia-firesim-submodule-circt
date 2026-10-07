// See LICENSE for license details.
// Exercise the production SimWrapper with flat/nested integer leaves in both
// bridge orientations. Keep this probe outside the immutable Rocket fixtures.
import firrtl._
import firrtl.ir._
import firrtl.annotations.ReferenceTarget
import midas.core.{SimWrapper, SimWrapperConfig}
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
import org.chipsalliance.cde.config.Parameters

object FAMEReadyValidPayloadOracle extends App {
  implicit val p: Parameters = Parameters.empty
  val destination = new java.io.File(args(0)); destination.mkdirs()
  val nested = args.lift(1).contains("nested")
  val payload = if (nested) "{ group : { x : SInt<3>, pad : SInt<0>, inner : { y : UInt<4>, valid : UInt<1> } }, valid : UInt<1> }"
                else "{ x : SInt<3>, pad : SInt<0>, y : UInt<5>, valid : UInt<1> }"
  val input = Parser.parse(s"""circuit Top :
    module Top :
      input hostClock : Clock
      input hostReset : UInt<1>
      output a : { flip ready : UInt<1>, valid : UInt<1>, bits : $payload }
      input ar : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1> }
      input b : { flip ready : UInt<1>, valid : UInt<1>, bits : $payload }
      output br : { flip ready : UInt<1>, valid : UInt<1>, bits : UInt<1> }
      input ticks : { flip ready : UInt<1>, valid : UInt<1>, bits : Clock }
      skip
  """)
  def target(name: String) = ReferenceTarget("Top", "Top", Nil, name, Nil).field("bits")
  val pairs = Seq(("send", "a", "ar", true), ("receive", "b", "br", false)).flatMap {
    case (name, port, reverse, source) =>
      val valid = target(port).field("valid")
      val ready = target(reverse)
      val paths = if (nested) Seq("group.x", "group.pad", "group.inner.y", "group.inner.valid", "valid")
                  else Seq("x", "pad", "y", "valid")
      // Endpoint order must not change the payload type or leaf binding.
      val fields = (if (nested) paths.reverse else paths).map(_.split("\\.").foldLeft(target(port))((t, f) => t.field(f)))
      val info = if (source) DecoupledForwardChannel.source(valid, ready)
                 else DecoupledForwardChannel.sink(valid, ready)
      Seq(FAMEChannelConnectionAnnotation(name + "_fwd", info, None,
            if (source) Some(fields) else None, if (source) None else Some(fields)),
          FAMEChannelConnectionAnnotation(name + "_rev", DecoupledReverseChannel, None,
            if (source) None else Some(Seq(ready)), if (source) Some(Seq(ready)) else None))
  }
  val annotations = pairs :+ FAMEChannelConnectionAnnotation("ticks",
    TargetClockChannel(Seq(RationalClock("clock", 1, 1)), Seq(1)), None, None, Some(Seq(target("ticks"))))
  val ports = input.modules.head.ports.map(port =>
    ReferenceTarget("Top", "Top", Nil, port.name, Nil) -> port).toMap
  val config = SimWrapperConfig(annotations, ports)
  val chirrtl = (new chisel3.stage.ChiselStage).emitChirrtl(new SimWrapper(config),
    Array("--target-dir", destination.getAbsolutePath))
  val circuit = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl), ChirrtlForm), Nil).circuit
  val wrapper = circuit.modules.collectFirst { case m: Module if m.name == circuit.main => m }.get
  for (port <- Seq("a", "b")) {
    val signed = wrapper.ports.find(_.name == s"channelPorts_${port}_bits_bits_x").get
    require(signed.tpe == SIntType(IntWidth(3)), s"$port lost signed payload type")
    val unsigned = wrapper.ports.find(_.name == s"channelPorts_${port}_bits_bits_${if (nested) "inner_y" else "y"}").get
    if (nested) require(wrapper.ports.find(_.name == s"channelPorts_${port}_bits_bits_inner_valid").get.tpe == UIntType(IntWidth(1)), "nested valid must remain data")
    require(unsigned.tpe == UIntType(IntWidth(if (nested) 4 else 5)), s"$port lost unsigned payload type")
  }
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(destination, name))
    try out.write(text) finally out.close()
  }
  write("post-fame.sfc.fir", input.serialize)
  write("post-fame.sfc.json", firrtl.annotations.JsonProtocol.serialize(annotations))
  write("signed-wrapper.sfc.fir", circuit.serialize)
  println(s"PASS ReadyValidChannel payloads: nested=$nested; signed/unsigned leaves, both orientations")
}
