// See LICENSE for license details.
// Elaborate the preserved actual ClockBridgeModule, including its countdown
// child, fastest-clock counter and Widget.genWideRORegInit snapshot bank.
import firrtl._
import firrtl.ir._
import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy.LazyModule
import firesim.lib.bridges.ClockParameters
import firesim.lib.bridgeutils.RationalClock
import firesim.lib.nasti.NastiParameters
import midas.widgets.{ClockBridgeModule, CtrlNastiKey}
object ClockBridgeOracle extends App {
  implicit val p: Parameters = Parameters.empty.alterPartial {
    case CtrlNastiKey => NastiParameters(32,32,12)
  }
  val cases = Seq(
    "single" -> Seq(2->2), "base-half" -> Seq(1->1,1->2),
    "two-three" -> Seq(1->2,1->3), "three-two" -> Seq(1->3,1->2),
    "two-three-four" -> Seq(1->2,1->3,1->4),
    "unreduced" -> Seq(2->4,3->9,4->16),
    "equal" -> Seq(2->2,3->3,2147483647->2147483647),
    "cross-product" -> Seq(2147483647->2147483646,2147483647->1073741823),
    "limit" -> Seq(1->1,1->65535))
  def flatten(s: Statement): Seq[Statement] = s match {
    case Block(stmts) => stmts.flatMap(flatten)
    case other => Seq(other)
  }
  for ((name, ratios) <- cases) {
    val clocks=ratios.zipWithIndex.map {case ((n,d),i)=> RationalClock(s"clock$i",n,d)}
    val owner=LazyModule(new ClockBridgeModule(ClockParameters(clocks)))
    val input=chisel3.stage.ChiselStage.emitChirrtl(owner.module)
    val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
    val writer=new java.io.PrintWriter(s"${args(0)}/$name.full-sfc.fir")
    try writer.write(low.circuit.serialize) finally writer.close()
    val modules=low.circuit.modules.collect {case m:Module=>m.name->m}.toMap
    val top=modules(low.circuit.main)
    val topBody=flatten(top.body)
    val generator=topBody.collectFirst {case i:DefInstance if i.name=="clockTokenGen"=>modules(i.module)}.get
    // Evaluate the actual top and countdown child. The decoded MCR boundary
    // is injected; Nasti transport remains the responsibility of its own tests.
    val bodies=Seq(""->topBody,"clockTokenGen."->flatten(generator.body))
    val nodes=bodies.flatMap {case (prefix,body)=>body.collect {
      case n:DefNode=>(prefix+n.name)->(n.value,prefix)}}.toMap
    val drivers=bodies.flatMap {case (prefix,body)=>body.collect {
      case c:Connect=>(prefix+c.loc.serialize)->(c.expr,prefix)}}.toMap
    val regs=bodies.flatMap {case (prefix,body)=>body.collect {case r:DefRegister=>(prefix,r)}}
    var state=regs.map {case (prefix,r)=>(prefix+r.name)->BigInt(0)}.toMap
    var inputs=Map.empty[String,BigInt]
    var memo=Map.empty[String,BigInt]
    def eval(e:Expression,prefix:String):BigInt = {
      val value=e match {
        case UIntLiteral(v,_)=>v
        case Mux(s,h,l,_)=>eval(if(eval(s,prefix)!=0)h else l,prefix)
        case DoPrim(op,a,c,_)=>op match {
          case PrimOps.Eq=>if(eval(a(0),prefix)==eval(a(1),prefix))BigInt(1) else BigInt(0)
          case PrimOps.Lt=>if(eval(a(0),prefix)<eval(a(1),prefix))BigInt(1) else BigInt(0)
          case PrimOps.Sub=>eval(a(0),prefix)-eval(a(1),prefix)
          case PrimOps.Add=>eval(a(0),prefix)+eval(a(1),prefix)
          case PrimOps.And=>eval(a(0),prefix)&eval(a(1),prefix)
          case PrimOps.Bits=>(eval(a(0),prefix)>>c(1).toInt)&((BigInt(1)<<(c(0)-c(1)+1).toInt)-1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt=>eval(a(0),prefix)
          case other=>sys.error(s"unsupported oracle primitive $other")
        }
        case other=>
          val k=prefix+other.serialize
          memo.getOrElse(k, {
            val v=state.getOrElse(k,inputs.getOrElse(k, {
              val (expr,scope)=nodes.getOrElse(k,drivers(k));eval(expr,scope)
            }))
            memo += k->v;v
          })
      }
      e.tpe match {
        case UIntType(IntWidth(w))=>value & ((BigInt(1)<<w.toInt)-1)
        case _=>value
      }
    }
    def value(k:String):BigInt = {
      val (e,prefix)=drivers(k);eval(e,prefix)
    }
    def clockLanes(e:Expression,seen:Set[String]=Set.empty):Set[Int] = e match {
      case Mux(s,h,l,_)=>clockLanes(s,seen)++clockLanes(h,seen)++clockLanes(l,seen)
      case DoPrim(_,a,_,_)=>a.flatMap(clockLanes(_,seen)).toSet
      case _:UIntLiteral=>Set.empty
      case other=>
        val k=other.serialize
        if(k.startsWith("hPort_clocks_bits_"))Set(k.stripPrefix("hPort_clocks_bits_").toInt)
        else if(seen(k) || state.contains(k))Set.empty
        else nodes.get(k).orElse(drivers.get(k)).map {case (v,_)=>clockLanes(v,seen+k)}.getOrElse(Set.empty)
    }
    val selected=clockLanes(drivers("tCycleFastest")._1)
    require(selected.size==1,s"ambiguous actual fastest-clock predicate $selected")
    println(s"FASTEST $name ${selected.head}")
    println(s"REGISTERS $name ${owner.module.crRegistry.getAllRegs.mkString(" ")}")
    val cycles=if(name=="limit")200000 else 4096
    for(cycle<-0 until cycles) {
      val reset=cycle<2 || cycle==cycles/2
      val ready=cycle%97>=13 && cycle%7!=0
      def bit(b:Boolean)=BigInt(if(b)1 else 0)
      inputs=Map("clock"->BigInt(0),"reset"->bit(reset),"hPort_clocks_ready"->bit(ready)) ++
        (0 until 6).flatMap {i=>Seq(
          s"crFile.io_mcr_write_${i}_valid"->bit((i==2&&cycle%19==0)||(i==5&&cycle%23==0)),
          s"crFile.io_mcr_write_${i}_bits"->BigInt(if(i==2&&cycle%3==0 || i==5&&cycle%5==0)2 else 1))}.toMap
      memo=Map.empty
      require(value("hPort_clocks_valid")==1)
      val mask=ratios.indices.map(i=>value(s"hPort_clocks_bits_$i")<<i).foldLeft(BigInt(0))(_|_)
      val host=state("hCycle"); val target=state("tCycleFastest")
      val savedHost=value("crFile.io_mcr_read_0_bits")|(value("crFile.io_mcr_read_1_bits")<<32)
      val savedTarget=value("crFile.io_mcr_read_3_bits")|(value("crFile.io_mcr_read_4_bits")<<32)
      println(s"BRIDGE $name $cycle $mask $host $target $savedHost $savedTarget")
      state=regs.map {case (prefix,r)=>
        val k=prefix+r.name
        k->(if(eval(r.reset,prefix)!=0)eval(r.init,prefix) else value(k))}.toMap
    }
  }
}
