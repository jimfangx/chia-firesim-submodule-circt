// See LICENSE for license details.
// Production SFC RemoveZeroWidth and Golden Gate exact annotation renaming.
import firrtl._
import firrtl.annotations._
import firrtl.transforms.DontTouchAnnotation
import midas.passes.fame._
object LowerTypesZeroWidthOracle extends App {
  val destination = new java.io.File(args(0)); destination.mkdirs()
  val circuit = Parser.parse("""circuit Top :
    module Top :
      input clock : Clock
      input io : { pad : UInt<0>, signed : SInt<0>, valid : UInt<1> }
      input empty : { pad : UInt<0>, signed : SInt<0> }
      node alias = io
      wire wire : { pad : UInt<0>, signed : SInt<0>, valid : UInt<1> }
      wire <= io
      reg state : { pad : UInt<0>, signed : SInt<0>, valid : UInt<1> }, clock
      state <= io
      wire inferred : UInt
      inferred <= io.pad
      skip
  """)
  def rt(name: String): ReferenceTarget = name.split("\\.").drop(1).foldLeft(
    ReferenceTarget("Top", "Top", Nil, name.takeWhile(_ != '.'), Nil))((t, f) => t.field(f))
  def write(name: String, text: String): Unit = {
    val out = new java.io.PrintWriter(new java.io.File(destination, name))
    try out.write(text) finally out.close()
  }
  val fanout = (Seq("io", "wire", "state").flatMap(n => Seq(n, n+".pad", n+".signed")) ++ Seq("alias", "empty", "inferred")).flatMap(n =>
    Seq(DontTouchAnnotation(rt(n)), FAMEHostReset(rt(n))))
  write("fanout.fir", circuit.serialize); write("fanout.json", JsonProtocol.serialize(fanout))
  val result = new LowFirrtlCompiler().compile(CircuitState(circuit, HighForm, fanout), Nil)
  val retained = result.annotations.collect { case a: DontTouchAnnotation => a; case a: FAMEHostReset => a }
  require(retained.size == 8 && retained.forall(_.getTargets.head.serialize.endsWith("_valid")))
  write("fanout.sfc.fir", result.circuit.serialize); write("fanout.sfc.json", JsonProtocol.serialize(retained))
  val zero = rt("io.pad"); val valid = rt("io.valid")
  val exact = Seq(
    "ports-inferred" -> FAMEChannelPortsAnnotation("payload", None, Seq(valid, rt("inferred"))),
    "ports" -> FAMEChannelPortsAnnotation("payload", None, Seq(valid, zero)),
    "clockPort" -> FAMEChannelPortsAnnotation("payload", Some(zero), Seq(valid)),
    "sources" -> FAMEChannelConnectionAnnotation.source("payload", PipeChannel(0), None, Seq(valid, zero)),
    "sinks" -> FAMEChannelConnectionAnnotation.sink("payload", PipeChannel(0), None, Seq(valid, zero)),
    "clock" -> FAMEChannelConnectionAnnotation.source("payload", PipeChannel(0), Some(zero), Seq(valid)),
    "readySink" -> DecoupledForwardChannel(Some(zero), None, None, None),
    "validSource" -> DecoupledForwardChannel(None, Some(zero), None, None),
    "readySource" -> DecoupledForwardChannel(None, None, Some(zero), None),
    "validSink" -> DecoupledForwardChannel(None, None, None, Some(zero)))
  for ((name, value) <- exact) {
    val annotation = value match {
      case a: Annotation => a
      case info: DecoupledForwardChannel => FAMEChannelConnectionAnnotation.source("payload", info, None, Seq(valid))
    }
    write(name+".json", JsonProtocol.serialize(Seq(annotation)))
    var rejected = false
    try new LowFirrtlCompiler().compile(CircuitState(circuit, HighForm, Seq(annotation)), Nil)
    catch { case e: AssertionError if e.getMessage.contains("renameMatches") => rejected = true }
    require(rejected, s"SFC accepted zero-width exact $name")
    println(s"PASS SFC rejects zero-width exact $name")
  }
  println("PASS SFC deletes zero-width DontTouch/host-reset fanout, including inferred wire")
}
