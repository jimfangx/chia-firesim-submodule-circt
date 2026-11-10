// See LICENSE for license details.
// Parse independently emitted circuits. No transform is run on native output.
import firrtl._
import firrtl.ir._
object FAMEModelGraphMemoryCompare extends App {
  require(args.length==2,"usage: MemoryCompare sfc-fame.fir native-fame.fir")
  def read(p:String)=Parser.parse(scala.io.Source.fromFile(p).mkString)
  val circuits=args.map(read)
  def flatten(s:Statement):Seq[Statement]=s match {case Block(ss)=>ss.flatMap(flatten);case other=>Seq(other)}
  def module(c:Circuit,n:String)=c.modules.collectFirst{case m:Module if m.name==n=>m}.get
  def interface(m:Module)=m.ports.map(p=>(p.name,p.direction.serialize,p.tpe.serialize)).sortBy(_._1)
  def connections(m:Module, memoryOnly:Boolean)= {
    val body=flatten(m.body);val nodes=body.collect{case n:DefNode=>n.name->n.value}.toMap
    def canonical(e:Expression):String=e match {
      case r:WRef if nodes.contains(r.name)=>canonical(nodes(r.name))
      case r:WRef=>r.name
      case f:WSubField=>f.serialize
      case UIntLiteral(v,_)=>v.toString
      case DoPrim(op,as,cs,_)=>s"$op(${as.map(canonical).mkString(",")};${cs.mkString(",")})"
      case Mux(s,h,l,_)=>s"mux(${canonical(s)},${canonical(h)},${canonical(l)})"
      case _=>e.serialize
    }
    body.collect{case c:Connect if !memoryOnly||c.loc.serialize.startsWith("asyncRam.")||c.loc.serialize.startsWith("syncRam.")=>c.loc.serialize->canonical(c.expr)}.toMap
  }
  for(name<-Seq("Top","Hub","Memory","Combinational")) {
    val pair=circuits.map(c=>module(c,name));require(interface(pair(0))==interface(pair(1)),s"$name channel interface mismatch")
  }
  val memories=circuits.map(c=>flatten(module(c,"Memory").body).collect{case d:DefMemory=>
    Seq(d.name,d.dataType.serialize,d.depth.toString,d.readLatency.toString,d.writeLatency.toString,d.readers.mkString(","),d.writers.mkString(","),d.readwriters.mkString(","),d.readUnderWrite.toString).mkString("|")}.sorted)
  require(memories(0)==memories(1) && memories(0).size==2,"target memory descriptors changed")
  val memoryConnections=circuits.map(c=>connections(module(c,"Memory"),true))
  require(memoryConnections(0)==memoryConnections(1),s"memory control/data/gated clock mismatch $memoryConnections")
  val topConnections=circuits.map(c=>connections(module(c,"Top"),false))
  require(topConnections(0)==topConnections(1),"complete transformed top wiring differs")
  for(c<-circuits; name<-Seq("Memory","Combinational")) {
    val m=module(c,name);val body=flatten(m.body)
    require(body.collect{case w:DefWire if w.name=="aliasClock"=>w}.size==1,s"$name clock alias was not retained as wire")
    require(connections(m,false)("aliasClock")=="clock_buffer.O",s"$name alias lost original gated-clock source")
  }
  println("PASS complete interfaces/top wiring, two exact target memory descriptors, every memory control/data/gated clock connection and both virtual-model clock alias wires")
}
