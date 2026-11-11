// See LICENSE for license details.
// Regenerate finite sharing fixtures using the SFC assembly JAR on the classpath.
// Run midas.passes.fame.MultiCycleRAMSharingOracle OUTPUT_DIRECTORY.
package midas.passes.fame
import firrtl._
import firrtl.ir._
import firrtl.annotations._
object MultiCycleRAMSharingOracle extends App {
 val out=new java.io.File(args(0));out.mkdirs()
 def save(n:String,t:String)={val p=new java.io.PrintWriter(new java.io.File(out,n));try p.write(t)finally p.close()}
 val u1=UIntType(IntWidth(1));val addr=UIntType(IntWidth(3));val data=UIntType(IntWidth(17));val mt=ModuleTarget("Probe","Probe")
 def payload(fs:(String,Type)*)=BundleType(fs.map{case(n,t)=>Field(n,Default,t)})
 def port(n:String,d:Direction,t:Type)=Port(NoInfo,n,d,Decouple(t))
 def bits(n:String)=mt.ref(n).field("bits")
 def field(n:String,f:String)=bits(n).field(f)
 val host=Seq(Port(NoInfo,"hostClock",Input,ClockType),Port(NoInfo,"hostReset",Input,u1))
 val cases=Seq("read-full-shared","read-addr-shared","rw-addr-shared","rw-full-shared")
 for(name<-cases){
  val full=name.contains("full");val twoReads=name.startsWith("read-")
  val shared=if(full)port("shared",Input,payload("addr"->addr,"en"->u1))else port("shared",Input,addr)
  val responses=(0 until (if(twoReads)2 else 1)).map(i=>port(s"response$i",Output,data))
  val ens=if(full)Seq.empty else(0 until(if(twoReads)2 else 1)).map(i=>port(s"en$i",Input,u1))
  val wfields=Seq("data"->data,"mask"->u1)++(if(twoReads)Seq("addr"->addr,"en"->u1)else if(!full)Seq("en"->u1)else Seq.empty)
  val write=port("write",Input,payload(wfields:_*))
  val ps=host++Seq(shared)++ens++responses++Seq(write)
  val rs=responses.indices.map(i=>ModelReadPort(addr=if(full)field("shared","addr")else bits("shared"),en=if(full)field("shared","en")else bits(s"en$i"),data=bits(s"response$i")))
  val w=ModelWritePort(addr=if(twoReads)field("write","addr")else if(full)field("shared","addr")else bits("shared"),en=if(!twoReads&&full)field("shared","en")else field("write","en"),data=field("write","data"),mask=field("write","mask"))
  val input=CircuitState(Circuit(NoInfo,Seq(Module(NoInfo,"Probe",ps,EmptyStmt)),"Probe"),LowForm,rs:+w)
  save(s"$name.input.fir",input.circuit.serialize);save(s"$name.annotations.json",JsonProtocol.serialize(input.annotations))
  val sfc=new EmitAndWrapRAMModels().runTransform(input)
  save(s"$name.sfc.fir",sfc.circuit.serialize)
  val emitted=new VerilogCompiler().compileAndEmit(sfc,Nil)
  save(s"$name.sfc.sv",emitted.getEmittedCircuit.value)
  println("PASS emit "+name)
 }
}
