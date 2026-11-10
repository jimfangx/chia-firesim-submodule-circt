// See LICENSE for license details.
// Derive the virtual-clock model using the complete unchanged FAMETransform,
// then execute complete unchanged MultiThreadFAME5Models on a canonical wrapper.
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import firrtl.annotations.JsonProtocol
import midas.passes.fame._
import firesim.lib.bridgeutils.RationalClock
object FAME5DynamicClockOracle extends App {
val input="""circuit Top :
 module Hub :
  input clock : Clock
  output alias : Clock
  input data : UInt<8>
  output out : UInt<8>
  out <= data
  alias <= clock
 module Worker :
  input clock : Clock
  input data : UInt<8>
  output out : UInt<8>
  output alias : Clock
  reg state : UInt<8>, clock with :
   reset => (bits(data, 7, 7), UInt<8>(0))
  state <= bits(add(state, bits(data, 6, 0)), 7, 0)
  out <= state
  alias <= clock
 module Top :
  input workerClock : Clock
  input hostClock : Clock
  input hostReset : UInt<1>
  input clock : Clock
  input data : UInt<8>
  output out : UInt<8>
  output alias : Clock
  input hdata : UInt<8>
  output halias : Clock
  output hout : UInt<8>
  inst h of Hub
  h.clock <= clock
  h.data <= hdata
  hout <= h.out
  halias <= h.alias
  inst w of Worker
  w.clock <= workerClock
  w.data <= data
  out <= w.out
  alias <= w.alias
"""
val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
val t=ModuleTarget("Top","Top");val h=ModuleTarget("Top","Hub");val w=ModuleTarget("Top","Worker")
val annos=Seq(FAMEHostClock(t.ref("hostClock")),FAMEHostReset(t.ref("hostReset")),FAMETransformAnnotation(h),FAMETransformAnnotation(w),FAMEChannelPortsAnnotation("tick",None,Seq(h.ref("clock"))),FAMEChannelConnectionAnnotation("tick",TargetClockChannel(Seq(RationalClock("clock",1,1)),Seq(1)),None,None,Some(Seq(t.ref("clock")))),FAMEChannelPortsAnnotation("input",None,Seq(w.ref("data"))),FAMEChannelPortsAnnotation("output",None,Seq(w.ref("out"))),FAMEChannelConnectionAnnotation("input",PipeChannel(0),Some(t.ref("halias")),None,Some(Seq(t.ref("data")))),FAMEChannelConnectionAnnotation("output",PipeChannel(0),Some(t.ref("halias")),Some(Seq(t.ref("out"))),None))
val hubAnnos=Seq(FAMEChannelPortsAnnotation("hin",Some(h.ref("alias")),Seq(h.ref("data"))),FAMEChannelPortsAnnotation("hout",Some(h.ref("alias")),Seq(h.ref("out"))),FAMEChannelConnectionAnnotation("hin",PipeChannel(0),Some(t.ref("halias")),None,Some(Seq(t.ref("hdata")))),FAMEChannelConnectionAnnotation("hout",PipeChannel(0),Some(t.ref("halias")),Some(Seq(t.ref("hout"))),None))
val result=new FAMETransform().execute(low.copy(annotations=annos++hubAnnos))
val destination = new java.io.File(args(0)); destination.mkdirs()
def write(name: String, value: String): Unit = {
  val out = new java.io.PrintWriter(new java.io.File(destination,name)); try out.write(value) finally out.close()
}
write("complete-fame.sfc.fir", result.circuit.serialize)
val worker = result.circuit.modules.collectFirst { case m: Module if m.name=="Worker" => m }.get
require(worker.ports.map(_.name)==Seq("hostClock","hostReset","input_sink","output_source"))
val gate = midas.passes.DefineAbstractClockGate.blackbox
val n=args(1).toInt; require(Seq(2,4).contains(n))
val order=Seq(1,0,3,2).filter(_<n)
val channelPorts=(0 until n).flatMap { i => worker.ports.drop(2).map { p =>
  p.copy(name=(if(p.direction==Input) "in" else "out")+i)
}}
val body=order.flatMap { i => Seq[Statement](
  WDefInstance(NoInfo,s"worker$i","Worker",UnknownType),
  Connect(NoInfo,WSubField(WRef(s"worker$i"),"hostClock"),WRef("hostClock")),
  Connect(NoInfo,WSubField(WRef(s"worker$i"),"hostReset"),WRef("hostReset")),
  Connect(NoInfo,WSubField(WRef(s"worker$i"),"input_sink"),WRef(s"in$i")),
  Connect(NoInfo,WRef(s"out$i"),WSubField(WRef(s"worker$i"),"output_source"))) }
val wrapper=Module(NoInfo,"Top",worker.ports.take(2)++channelPorts,Block(body))
val circuit=Circuit(NoInfo,Seq(worker,wrapper,gate),"Top")
val selector=midas.targetutils.FirrtlEnableModelMultiThreadingAnnotation(ModuleTarget("Top","Top").instOf("worker0","Worker"))
val annotations=Seq(selector)
val config=midas.stage.phases.ConfigParametersAnnotation(new midas.SimConfig().toInstance.alterPartial {case midas.EnableModelMultiThreading => true})
val high=new HighFirrtlCompiler().compile(CircuitState(circuit,HighForm,annotations:+config),Nil)
write("input.fir",high.circuit.serialize)
write("input.anno.json",JsonProtocol.serialize(annotations))
val grouped=MultiThreadFAME5Models.execute(high)
write("threaded.sfc.fir",grouped.circuit.serialize)
write("threaded.sfc.json",JsonProtocol.serialize(grouped.annotations.filterNot(_.isInstanceOf[midas.stage.phases.ConfigParametersAnnotation])))
// Production MidasTransforms applies the unchanged Xilinx specialization after
// grouping. Keep the original abstract-gate oracle as the default so its
// counterexample is retained rather than hidden by changing the oracle.
val emitState = if (args.drop(2).contains("xilinx")) {
  val lowGrouped = new LowFirrtlCompiler().compile(grouped.copy(form=HighForm),Nil)
  val specialized = midas.passes.xilinx.HostSpecialization.execute(lowGrouped)
  write("threaded.xilinx.fir",specialized.circuit.serialize)
  specialized.copy(form=LowForm)
} else grouped.copy(form=HighForm)
val rtl=new VerilogCompiler().compileAndEmit(emitState,Nil)
write("threaded.sfc.v",rtl.getEmittedCircuit.value)
write("AbstractClockGate.v",midas.passes.DefineAbstractClockGate.impl)
println(s"SFC complete FAMETransform-derived virtual-clock Worker grouped $n threads")
}
