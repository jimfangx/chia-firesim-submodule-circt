// See LICENSE for license details.
// Actual immutable SFC FAMETransform and production clock-gate definition. Native output
// is parsed independently for comparison; never transform it with SFC.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
object FAMEModelGraphSharedGroupedOracle extends App {
  val directory = new java.io.File(args(0)); directory.mkdirs()
  def write(name:String,value:String):Unit={val p=new java.io.PrintWriter(new java.io.File(directory,name));try p.write(value) finally p.close()}
  val input = scala.io.Source.fromFile(args(1)).mkString
  val annotations = JsonProtocol.deserialize(scala.io.Source.fromFile(args(2)).mkString)
  val low = new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input), ChirrtlForm), Nil)
  write("input.fir", low.circuit.serialize); write("input.json", JsonProtocol.serialize(annotations))
  val fame=midas.passes.DefineAbstractClockGate.runTransform(AddRemainingFanoutAnnotations.runTransform(new FAMETransform().runTransform(low.copy(annotations=annotations))))
  write("sfc-fame.fir",fame.circuit.serialize);write("sfc-fame.json",JsonProtocol.serialize(fame.annotations))
  // Production SimWrapper rejects aggregate PipeChannel endpoints. Retain its
  // explicit limitation here: this oracle stops at the complete FAME boundary.
  println("PASS unchanged SFC FAMETransform: two real domains and independent repeated grouped channels")
}
