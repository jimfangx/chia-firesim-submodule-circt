// See LICENSE for license details.
package midas.passes.fame
import firrtl._
import firrtl.annotations._
object FAMEModelGraphVirtualOracle extends App {
  val directory=new java.io.File(args(0));directory.mkdirs()
  def write(name:String,text:String):Unit={val p=new java.io.PrintWriter(new java.io.File(directory,name));try p.write(text) finally p.close()}
  val input=scala.io.Source.fromFile(args(1)).mkString
  val annotations=JsonProtocol.deserialize(scala.io.Source.fromFile(args(2)).mkString)
  val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
  write("input.fir",low.circuit.serialize);write("input.json",JsonProtocol.serialize(annotations))
  val fame=midas.passes.DefineAbstractClockGate.runTransform(new FAMETransform().runTransform(low.copy(annotations=annotations)))
  write("sfc-fame.fir",fame.circuit.serialize);write("sfc-fame.json",JsonProtocol.serialize(fame.annotations))
  val verilog=new VerilogCompiler().compileAndEmit(CircuitState(fame.circuit,HighForm),Nil)
  write("sfc.sv",verilog.getEmittedCircuit.value)
  println("PASS unchanged SFC hubless shared scalar VirtualClockChannel model graph and actual Scala VerilogCompiler")
}
