// See LICENSE for license details.
// Run with the pinned SFC assembly JAR: midas.passes.fame.MultiCycleRAMResponseJoinOracle OUTPUT.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
object MultiCycleRAMResponseJoinOracle extends App {
 val out=new java.io.File(args(0));out.mkdirs()
 def write(n:String,s:String)={val p=new java.io.PrintWriter(new java.io.File(out,n));try p.write(s) finally p.close()}
 val bit=UIntType(IntWidth(1)); val addr=UIntType(IntWidth(2));val data=UIntType(IntWidth(8))
 def payload(xs:(String,Type)*)=BundleType(xs.map{case(n,t)=>Field(n,Default,t)})
 val ports=Seq(Port(NoInfo,"hostClock",Input,ClockType),Port(NoInfo,"hostReset",Input,bit),
  Port(NoInfo,"rq0",Input,Decouple(payload("addr"->addr,"en"->bit))),
  Port(NoInfo,"rq1",Input,Decouple(payload("addr"->addr,"en"->bit))),
  Port(NoInfo,"responses",Output,Decouple(payload("d0"->data,"d1"->data))),
  Port(NoInfo,"write",Input,Decouple(payload("addr"->addr,"en"->bit,"data"->data,"mask"->bit))))
 val m=Module(NoInfo,"Grouped",ports,EmptyStmt);val mt=ModuleTarget("Grouped","Grouped")
 def r(i:Int)=ModelReadPort(addr=mt.ref("rq"+i).field("bits").field("addr"),en=mt.ref("rq"+i).field("bits").field("en"),data=mt.ref("responses").field("bits").field("d"+i))
 val w=ModelWritePort(addr=mt.ref("write").field("bits").field("addr"),en=mt.ref("write").field("bits").field("en"),data=mt.ref("write").field("bits").field("data"),mask=mt.ref("write").field("bits").field("mask"))
 val input=CircuitState(Circuit(NoInfo,Seq(m),"Grouped"),LowForm,Seq(r(0),r(1),w))
 write("input.fir",input.circuit.serialize);write("annotations.json",JsonProtocol.serialize(input.annotations))
 val reference=new EmitAndWrapRAMModels().runTransform(input)
 write("sfc.fir",reference.circuit.serialize);write("sfc.json",JsonProtocol.serialize(reference.annotations))
 val rtl=new VerilogCompiler().compileAndEmit(reference,Nil)
 write("sfc.sv",rtl.getEmittedCircuit.value)
 println("PASS grouped response wrapper emitted and lowered by SFC")
}
