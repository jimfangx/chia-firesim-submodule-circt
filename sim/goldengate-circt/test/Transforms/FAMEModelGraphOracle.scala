// See LICENSE for license details.
// Actual immutable SFC FAMETransform plus production SimWrapper. Native output
// is parsed independently for comparison; never transform it with SFC.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
import midas.core.{SimWrapper, SimWrapperConfig}
import firesim.lib.bridgeutils.RationalClock
import org.chipsalliance.cde.config.Parameters
object FAMEModelGraphOracle extends App {
  val directory = new java.io.File(args(0)); directory.mkdirs()
  def write(name:String,value:String):Unit={val p=new java.io.PrintWriter(new java.io.File(directory,name));try p.write(value) finally p.close()}
  val input = """circuit Top :
  module Hub :
    input c0 : Clock
    input c1 : Clock
    input data : UInt<8>
    input right : UInt<8>
    output out : UInt<8>
    output otherOut : UInt<8>
    output alias0 : Clock
    output alias1 : Clock
    reg state0 : UInt<8>, c0 with :
      reset => (bits(data, 7, 7), UInt<8>(0))
    reg state1 : UInt<8>, c1 with :
      reset => (bits(right, 7, 7), UInt<8>(0))
    state0 <= bits(add(state0, bits(data, 6, 0)), 7, 0)
    state1 <= bits(add(state1, bits(right, 6, 0)), 7, 0)
    out <= bits(add(state0, bits(data, 6, 0)), 7, 0)
    otherOut <= bits(add(state1, bits(right, 6, 0)), 7, 0)
    alias0 <= c0
    alias1 <= c1
  module Worker :
    input clock : Clock
    input data : UInt<8>
    output out : UInt<8>
    reg state : UInt<8>, clock with :
      reset => (bits(data, 7, 7), UInt<8>(0))
    state <= bits(add(state, bits(data, 6, 0)), 7, 0)
    out <= bits(add(state, bits(data, 6, 0)), 7, 0)
  module Top :
    input hostClock : Clock
    input hostReset : UInt<1>
    input c0 : Clock
    input c1 : Clock
    input h_data : UInt<8>
    input h_right : UInt<8>
    output h_out : UInt<8>
    output h_other0 : UInt<8>
    output h_other1 : UInt<8>
    output h_alias0 : Clock
    output h_alias1 : Clock
    input w0_clock : Clock
    input w0_data : UInt<8>
    output w0_out : UInt<8>
    input w1_clock : Clock
    input w1_data : UInt<8>
    output w1_out : UInt<8>
    inst h of Hub
    inst w0 of Worker
    inst w1 of Worker
    h.c0 <= c0
    h.c1 <= c1
    h.data <= h_data
    h.right <= h_right
    h_out <= h.out
    h_other0 <= h.otherOut
    h_other1 <= h.otherOut
    h_alias0 <= h.alias0
    h_alias1 <= h.alias1
    w0.clock <= w0_clock
    w0.data <= w0_data
    w0_out <= w0.out
    w1.clock <= w1_clock
    w1.data <= w1_data
    w1_out <= w1.out
"""
  val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
  val top=ModuleTarget("Top","Top");val hub=ModuleTarget("Top","Hub");val worker=ModuleTarget("Top","Worker")
  def boundary(name:String,p:String,clock:String,in:Boolean)=FAMEChannelConnectionAnnotation(name,PipeChannel(0),Some(top.ref(clock)),
    if(in)None else Some(Seq(top.ref(p))),if(in)Some(Seq(top.ref(p))) else None)
  val annotations:Seq[Annotation]=Seq(
    FAMEHostClock(top.ref("hostClock")),FAMEHostReset(top.ref("hostReset")),
    FAMETransformAnnotation(hub),FAMETransformAnnotation(worker),
    FAMEChannelPortsAnnotation("tick",None,Seq(hub.ref("c0"),hub.ref("c1"))),
    FAMEChannelConnectionAnnotation("tick",TargetClockChannel(Seq(RationalClock("d0",1,2),RationalClock("d1",1,3)),Seq(2,3)),None,None,Some(Seq(top.ref("c0"),top.ref("c1")))),
    FAMEChannelPortsAnnotation("input",Some(hub.ref("alias0")),Seq(hub.ref("data"))),
    FAMEChannelPortsAnnotation("right",Some(hub.ref("alias1")),Seq(hub.ref("right"))),
    FAMEChannelPortsAnnotation("output",Some(hub.ref("alias0")),Seq(hub.ref("out"))),
    FAMEChannelPortsAnnotation("other",Some(hub.ref("alias1")),Seq(hub.ref("otherOut"))),
    FAMEChannelPortsAnnotation("input",None,Seq(worker.ref("data"))),
    FAMEChannelPortsAnnotation("output",None,Seq(worker.ref("out"))),
    boundary("h_input","h_data","h_alias0",true),boundary("h_right","h_right","h_alias1",true),
    boundary("h_output","h_out","h_alias0",false),
    FAMEChannelConnectionAnnotation("h_to_w0",PipeChannel(1),Some(top.ref("h_alias1")),Some(Seq(top.ref("h_other0"))),Some(Seq(top.ref("w0_data")))),
    FAMEChannelConnectionAnnotation("h_to_w1",PipeChannel(0),Some(top.ref("h_alias1")),Some(Seq(top.ref("h_other1"))),Some(Seq(top.ref("w1_data")))),
    boundary("w0_output","w0_out","h_alias1",false),boundary("w1_output","w1_out","h_alias1",false),
    firrtl.transforms.DontTouchAnnotation(worker.ref("state")),firrtl.transforms.DontTouchAnnotation(top.ref("h_data")))
  write("input.fir",low.circuit.serialize);write("input.json",JsonProtocol.serialize(annotations))
  val fame=AddRemainingFanoutAnnotations.runTransform(new FAMETransform().runTransform(low.copy(annotations=annotations)))
  write("sfc-fame.fir",fame.circuit.serialize);write("sfc-fame.json",JsonProtocol.serialize(fame.annotations))
  implicit val parameters:Parameters=Parameters.empty
  val target=fame.circuit.modules.find(_.name==fame.circuit.main).get
  val types=target.ports.map(p=>ModuleTarget(fame.circuit.main,target.name).ref(p.name)->p).toMap
  val chirrtl=(new chisel3.stage.ChiselStage).emitChirrtl(new SimWrapper(SimWrapperConfig(fame.annotations,types)),Array("--target-dir",directory.getAbsolutePath))
  val wrapper=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(chirrtl),ChirrtlForm),Nil).circuit
  write("sfc-wrapper.fir",wrapper.serialize)
  println("PASS immutable SFC FAMETransform and SimWrapper: two real domains, shared repeated Worker, internal fanout queues with latencies0/1")
}
