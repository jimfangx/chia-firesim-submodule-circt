// See LICENSE for license details.
// Execute complete unchanged SFC grouping including recursive child threading.
import firrtl._
import firrtl.annotations.JsonProtocol
import midas.passes.fame._
object FAME5HierarchyOracle extends App {
 def read(path:String):String={val in=scala.io.Source.fromFile(path);try in.mkString finally in.close()}
 val directory=new java.io.File(args(2));directory.mkdirs()
 def write(name:String,value:String):Unit={val out=new java.io.PrintWriter(new java.io.File(directory,name));try out.write(value) finally out.close()}
 val config=midas.stage.phases.ConfigParametersAnnotation(new midas.SimConfig().toInstance.alterPartial{case midas.EnableModelMultiThreading=>true})
 val annotations=JsonProtocol.deserialize(read(args(1)),true):+config
 val high=new HighFirrtlCompiler().compile(CircuitState(Parser.parse(read(args(0))),ChirrtlForm,annotations),Nil)
 val result=MultiThreadFAME5Models.execute(high)
 write("threaded.sfc.fir",result.circuit.serialize)
 write("threaded.sfc.json",JsonProtocol.serialize(result.annotations.filterNot(_.isInstanceOf[midas.stage.phases.ConfigParametersAnnotation])))
 val emitState = if (args.drop(3).contains("xilinx")) {
   val low = new LowFirrtlCompiler().compile(result.copy(form=HighForm),Nil)
   val specialized = midas.passes.xilinx.HostSpecialization.execute(low)
   write("threaded.xilinx.fir",specialized.circuit.serialize)
   specialized.copy(form=LowForm)
 } else result.copy(form=HighForm)
 val rtl=new VerilogCompiler().compileAndEmit(emitState,Nil)
 write("threaded.sfc.v",rtl.getEmittedCircuit.value)
 println("Complete unchanged SFC recursive hierarchy grouping emitted")
}
