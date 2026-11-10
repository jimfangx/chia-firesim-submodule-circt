// See LICENSE for license details.
// Compare complete emitted SimWrapper connection cones for the fixed matched
// graph fixture. Explicit aliases describe its original port/local-channel
// distinction; no transformation or SFC lowering is applied to native FIRRTL.
import firrtl._
import firrtl.ir._
object FAMEModelGraphTransportCompare extends App {
  require(args.length==2,"usage: GraphTransportCompare sfc-wrapper.fir native-full.fir")
  def flatten(s:Statement):Seq[Statement]=s match{case Block(ss)=>ss.flatMap(flatten);case x=>Seq(x)}
  val circuits=args.map(p=>Parser.parse(scala.io.Source.fromFile(p).mkString))
  val wrappers=circuits.map(c=>c.modules.collectFirst{case m:Module if m.name==c.main=>m}.get)
  val aliases=Seq("h__otherOut_source"->"h__other_source","h__data_sink"->"h__input_sink","h__out_source"->"h__output_source",
    "w0__data_sink"->"w0__input_sink","w1__data_sink"->"w1__input_sink","w0__out_source"->"w0__output_source","w1__out_source"->"w1__output_source")
  def normalize(raw:String,sfc:Boolean):String={
    var s=raw
    for((from,to)<-aliases)s=s.replace(from,to)
    if(sfc){
      s=s.replace("target.h__tick_sink_bits__0","target.h__tick_sink.bits.c0").replace("target.h__tick_sink_bits__1","target.h__tick_sink.bits.c1")
      s=s.replace("channelPorts_h__tick_sink_bits_0","external.h__tick_sink.bits[0]").replace("channelPorts_h__tick_sink_bits_1","external.h__tick_sink.bits[1]")
      s=s.replace("channelPorts_","external.").replaceAll("_(ready|valid|bits)$", ".$1")
      if(s=="clock")s="external.hostClock";if(s=="reset")s="external.hostReset"
    }else{
      s=s.replace("target_FAMETop.","target.")
      if(!s.contains(".")&&s=="hostClock")s="external.hostClock"
      else if(!s.contains(".")&&s=="hostReset")s="external.hostReset"
      else if(!s.startsWith("target.") && !s.startsWith("PipeChannel_") && !s.startsWith("external."))s="external."+s
    }
    s.replaceAll("(\\.io_(?:in|out))_(ready|valid|bits)$", "$1.$2")
  }
  def cones(m:Module,sfc:Boolean):Map[String,String]={
    val body=flatten(m.body);val nodes=body.collect{case n:DefNode=>n.name->n.value}.toMap
    def canonical(e:Expression):String=e match{
      case r:WRef if nodes.contains(r.name)=>canonical(nodes(r.name))
      case r:WRef=>normalize(r.name,sfc)
      case r:WSubField=>normalize(r.serialize,sfc)
      case r:WSubIndex=>normalize(r.serialize,sfc)
      case UIntLiteral(v,_)=>v.toString
      case DoPrim(op,args,cs,_) if op==PrimOps.And||op==PrimOps.Or=>s"$op(${args.map(canonical).sorted.mkString(",")})"
      case DoPrim(op,args,cs,_)=>s"$op(${args.map(canonical).mkString(",")};${cs.mkString(",")})"
      case _=>sys.error("unsupported transport expression "+e.serialize)
    }
    body.collect{case c:Connect=>normalize(c.loc.serialize,sfc)->canonical(c.expr)}.toMap
  }
  val a=cones(wrappers(0),true);val b=cones(wrappers(1),false)
  val different=(a.keySet++b.keySet).toSeq.sorted.filter(k=>a.get(k)!=b.get(k))
  require(different.isEmpty,different.map(k=>s"$k: SFC=${a.get(k)} native=${b.get(k)}").mkString("wrapper connection mismatch\n","\n",""))
  def queues(c:Circuit,m:Module,sfc:Boolean):Map[String,(BigInt,Boolean)]={
    flatten(m.body).collect{case i:WDefInstance if i.name.startsWith("PipeChannel_")=>
      val q=c.modules.collectFirst{case x:Module if x.name==i.module=>x}.get
      val width=q.ports.find(_.name=="io_in_bits").get.tpe.asInstanceOf[UIntType].width.asInstanceOf[IntWidth].width
      val initial=flatten(q.body).exists{case r:DefRegister=>r.name=="initializing";case _=>false}
      i.name->(width,initial)}.toMap
  }
  val qa=queues(circuits(0),wrappers(0),true);val qb=queues(circuits(1),wrappers(1),false)
  require(qa==qb&&qa.size==7&&qa.values.count(_._2)==1,s"queue width/initial token mismatch $qa vs $qb")
  println(s"PASS ${a.size} complete wrapper connection cones and seven typed latency0/1 queues: internal fanout atomics, external interfaces, host reset/clock, clock-token Boolean lane order")
}
