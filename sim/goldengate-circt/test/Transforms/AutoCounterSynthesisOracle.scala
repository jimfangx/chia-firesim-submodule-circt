// See LICENSE for license details.
package midas.passes
import firrtl._
import firrtl.annotations.{JsonProtocol,NoTargetAnnotation,HasSerializationHints}
import firrtl.options.TargetDirAnnotation
import midas.widgets.EventMetadata
import midas.targetutils.PerfCounterOps
/** Execute the unchanged CSV target transform on the exact native fixture.
  * Serialization hints preserve actual operation singleton types after the
  * standalone constructor boundary, never infer them from empty JSON objects.
  */
object AutoCounterSynthesisOracle extends App {
  require(args.length==3,"input.fir input.json output-directory")
  def read(p:String):String={val s=scala.io.Source.fromFile(p);try s.mkString finally s.close()}
  val output=new java.io.File(args(2));require(output.mkdirs()||output.isDirectory)
  val state=CircuitState(Parser.parse(read(args(0))),LowForm,JsonProtocol.deserialize(read(args(1))) :+ TargetDirAnnotation(output.getPath))
  val transformed=new AutoCounterTransform().doTransform(state,usePrintfImplementation=false)
  val result=new ResolveAndCheck().runTransform(transformed)
  case class ConstructorHints() extends NoTargetAnnotation with HasSerializationHints {
    def typeHints=Seq(classOf[EventMetadata],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  def write(name:String,text:String):Unit={val out=new java.io.PrintWriter(new java.io.File(output,name));try out.write(text) finally out.close()}
  write("post-autocounter.fir",result.circuit.serialize)
  write("post-autocounter.json",JsonProtocol.serialize(result.annotations :+ ConstructorHints()))
  println("PASS unchanged SFC AutoCounterTransform CSV strategy on matched target FIRRTL/annotations")
}
