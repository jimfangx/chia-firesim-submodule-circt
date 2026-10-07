// See LICENSE for license details.
// Execute the unchanged production annotation transform on the native fixture.
import firrtl._
import firrtl.annotations.ReferenceTarget
import midas.passes.fame._
object RemainingFanoutOracle extends App {
  def target(ref: String) = ReferenceTarget("Top", "Top", Nil, ref, Nil)
  val a = target("producer").field("a")
  val b = target("producer").field("b")
  val other = target("other")
  def channel(name: String, sources: Seq[ReferenceTarget], latency: Int = 0) =
    FAMEChannelConnectionAnnotation(name, PipeChannel(latency),
      if (latency == 0) None else Some(target("input")), Some(sources),
      if (latency == 0) None else Some(Seq(target("input"))))
  val channels = Seq(channel("a0", Seq(a)), channel("b0", Seq(other), 1),
    channel("a1", Seq(a), 1), channel("a0", Seq(a)),
    channel("ordered0", Seq(a, b)), channel("reversed0", Seq(b, a)),
    channel("ordered1", Seq(a, b), 1), channel("b1", Seq(other)),
    FAMEChannelConnectionAnnotation("rv", DecoupledForwardChannel(None, None, None, None),
      None, Some(Seq(a)), None),
    FAMEChannelConnectionAnnotation("bridge0", PipeChannel(0), None, None, None),
    FAMEChannelConnectionAnnotation("bridge1", PipeChannel(0), None, None, None),
    FAMEChannelFanoutAnnotation(Seq("bridge1", "bridge0")))
  val circuit = Parser.parse("circuit Top :\n  module Top :\n    output producer : { a : UInt<1>, b : UInt<1> }\n    output other : UInt<1>\n    input input : UInt<1>\n    producer.a <= UInt<1>(0)\n    producer.b <= UInt<1>(0)\n    other <= UInt<1>(0)\n")
  val input = CircuitState(circuit, LowForm, channels)
  val output = AddRemainingFanoutAnnotations.execute(input)
  require(output.annotations.take(channels.size) == channels)
  val groups = output.annotations.drop(channels.size).map {
    case FAMEChannelFanoutAnnotation(names) => names.mkString(",")
    case other => sys.error(s"unexpected generated annotation: $other")
  }
  require(groups == Seq("a0,a1", "b0,b1", "ordered0,ordered1"))
  groups.foreach(names => println(s"GROUP $names"))
  require(AddRemainingFanoutAnnotations.execute(output).annotations.size == output.annotations.size+3)
  println("PASS production AddRemainingFanoutAnnotations: ordered groups and preservation")
}
