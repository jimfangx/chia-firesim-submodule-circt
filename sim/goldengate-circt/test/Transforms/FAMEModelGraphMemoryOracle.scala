// See LICENSE for license details.
// Run unchanged FAMETransform on the exact same prepared scalar graph as native.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.core.{SimWrapper, SimWrapperConfig}
import org.chipsalliance.cde.config.Parameters
object FAMEModelGraphMemoryOracle extends App {
  val directory = new java.io.File(args(0)); directory.mkdirs()
  def write(name:String,value:String):Unit={val p=new java.io.PrintWriter(new java.io.File(directory,name));try p.write(value) finally p.close()}
  val input=scala.io.Source.fromFile(args(1)).mkString
  val annotations=JsonProtocol.deserialize(scala.io.Source.fromFile(args(2)).mkString)
  val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
  write("input.fir",low.circuit.serialize); write("input.json",JsonProtocol.serialize(annotations))
  val fame=midas.passes.DefineAbstractClockGate.runTransform(AddRemainingFanoutAnnotations.runTransform(new FAMETransform().runTransform(low.copy(annotations=annotations))))
  write("sfc-fame.fir",fame.circuit.serialize); write("sfc-fame.json",JsonProtocol.serialize(fame.annotations))
  val verilog=new VerilogCompiler().compileAndEmit(CircuitState(fame.circuit,HighForm),Nil)
  write("sfc.sv",verilog.getEmittedCircuit.value)
  implicit val parameters:Parameters=Parameters.empty
  val target=fame.circuit.modules.find(_.name==fame.circuit.main).get
  val types=target.ports.map(p=>ModuleTarget(fame.circuit.main,target.name).ref(p.name)->p).toMap
  val chirrtl=(new chisel3.stage.ChiselStage).emitChirrtl(new SimWrapper(SimWrapperConfig(fame.annotations,types)),Array("--target-dir",directory.getAbsolutePath))
  val wrapper=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl),ChirrtlForm),Nil).circuit
  write("sfc-wrapper.fir",wrapper.serialize)
  println("PASS unchanged SFC complete FAMETransform and Pipe SimWrapper: memory and combinational virtual models with exported clock aliases")
}
