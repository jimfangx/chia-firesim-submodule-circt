// See LICENSE for license details.
// Same prepared graph input: execute unchanged SFC FAMETransform, grouping,
// SimWrapper and Xilinx specialization. Native IR is never lowered with SFC.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.core.{SimWrapper, SimWrapperConfig}
import midas.stage.phases.ConfigParametersAnnotation
import org.chipsalliance.cde.config.Parameters
object FAME5ModelGraphOracle extends App {
  require(args.length==3,"usage: FAME5ModelGraphOracle input.fir input.json output-directory")
  def read(path:String):String={val source=scala.io.Source.fromFile(path);try source.mkString finally source.close()}
  val directory=new java.io.File(args(2));directory.mkdirs()
  def write(name:String,value:String):Unit={val out=new java.io.PrintWriter(new java.io.File(directory,name));try out.write(value) finally out.close()}
  val config=ConfigParametersAnnotation(new midas.SimConfig().toInstance.alterPartial {case midas.EnableModelMultiThreading=>true})
  val annotations=JsonProtocol.deserialize(read(args(1))) :+ config
  val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(read(args(0))),ChirrtlForm,annotations),Nil)
  val transformed=new FAMETransform().runTransform(low)
  val prepared=midas.passes.DefineAbstractClockGate.runTransform(AddRemainingFanoutAnnotations.runTransform(transformed))
  val resolved=new firrtl.ResolveAndCheck().runTransform(prepared)
  write("sfc-pre-fame5.fir",resolved.circuit.serialize)
  val grouped=MultiThreadFAME5Models.runTransform(resolved)
  write("sfc-post-fame5-unresolved.fir",grouped.circuit.serialize)
  write("sfc-post-fame5-unresolved.json",JsonProtocol.serialize(grouped.annotations.filterNot(_.isInstanceOf[ConfigParametersAnnotation])))
  val boundary=new firrtl.ResolveAndCheck().runTransform(grouped)
  val emittedAnnotations=boundary.annotations.filterNot(_.isInstanceOf[ConfigParametersAnnotation])
  write("sfc-fame.fir",boundary.circuit.serialize)
  write("sfc-fame.json",JsonProtocol.serialize(emittedAnnotations))
  val specialized=midas.passes.xilinx.HostSpecialization.runTransform(new LowFirrtlCompiler().compile(boundary.copy(form=HighForm),Nil))
  write("sfc-xilinx.fir",specialized.circuit.serialize)
  // Preserve a legacy backend annotation failure verbatim if present; still
  // retain the independently inspectable SFC transformed circuit and wrapper.
  // A recorded failure is not a passing SFC syntax/feature receipt.
  try write("sfc.sv",new VerilogCompiler().compileAndEmit(specialized.copy(form=LowForm),Nil).getEmittedCircuit.value)
  catch {case e:Exception=>write("sfc-verilog-failure.txt",e.toString+"\n")}
  implicit val parameters:Parameters=Parameters.empty
  val target=boundary.circuit.modules.find(_.name==boundary.circuit.main).get
  val types=target.ports.map(p=>ModuleTarget(boundary.circuit.main,target.name).ref(p.name)->p).toMap
  val chirrtl=(new chisel3.stage.ChiselStage).emitChirrtl(new SimWrapper(SimWrapperConfig(emittedAnnotations,types)),Array("--target-dir",directory.getAbsolutePath))
  val wrapper=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl),ChirrtlForm),Nil).circuit
  write("sfc-wrapper.fir",wrapper.serialize)
  println("EMITTED unchanged SFC FAMETransform -> MultiThreadFAME5Models -> SimWrapper/Xilinx specialization; inspect sfc-verilog-failure.txt before any syntax claim")
}
