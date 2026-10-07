// See LICENSE for license details.
// Elaborate and execute the preserved actual Scala ClockBridge token generator.
import firrtl._
import firrtl.ir._
import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy.LazyModule
import firesim.lib.bridges.{ClockParameters, FindScaledPeriodGCD}
import firesim.lib.bridgeutils.RationalClock
import firesim.lib.nasti.NastiParameters
import midas.widgets.{ClockBridgeModule, CtrlNastiKey}
object RationalClockOracle extends App {
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
    val input=chisel3.stage.ChiselStage.emitChirrtl(new owner.RationalClockTokenGenerator(ratios))
    val low=new LowFirrtlCompiler().compile(CircuitState(Parser.parse(input),ChirrtlForm),Nil)
    if (args.nonEmpty) {
      val writer=new java.io.PrintWriter(s"${args(0)}/$name.sfc.fir")
      try writer.write(low.circuit.serialize) finally writer.close()
    }
    val m=low.circuit.modules.collectFirst {case m:Module if m.name==low.circuit.main=>m}.get
    val body=flatten(m.body)
    val nodes=body.collect {case n:DefNode => n.name -> n.value}.toMap
    val drivers=body.collect {case c:Connect => c.loc.serialize -> c.expr}.toMap
    val regs=body.collect {case r:DefRegister => r}
    var state=regs.map(r=>r.name->BigInt(0)).toMap
    var inputs=Map.empty[String,BigInt]
    var memo=Map.empty[String,BigInt]
    def eval(e:Expression):BigInt = {
      val value=e match {
        case UIntLiteral(v,_)=>v
        case Mux(s,h,l,_)=>eval(if(eval(s)!=0)h else l)
        case DoPrim(op,a,c,_)=>op match {
          case PrimOps.Eq=>if(eval(a(0))==eval(a(1)))BigInt(1) else BigInt(0)
          case PrimOps.Lt=>if(eval(a(0))<eval(a(1)))BigInt(1) else BigInt(0)
          case PrimOps.Sub=>eval(a(0))-eval(a(1))
          case PrimOps.Bits=>(eval(a(0))>>c(1).toInt)&((BigInt(1)<<(c(0)-c(1)+1).toInt)-1)
          case PrimOps.Tail | PrimOps.Pad | PrimOps.AsUInt=>eval(a(0))
          case other=>sys.error(s"unsupported oracle primitive $other")
        }
        case other=>
          val k=other.serialize
          memo.getOrElse(k, {
            val v=state.getOrElse(k,inputs.getOrElse(k,eval(nodes.getOrElse(k,drivers(k)))))
            memo += k->v;v
          })
      }
      e.tpe match {
        case UIntType(IntWidth(w))=>value & ((BigInt(1)<<w.toInt)-1)
        case _=>value
      }
    }
    val periods=FindScaledPeriodGCD(ratios)
    val width=periods.map(p=>p.bitLength).max
    require(regs.forall(_.tpe==UIntType(IntWidth(width))))
    println(s"PERIOD $name $width ${periods.mkString(" ")}")
    val cycles=if(name=="limit")200000 else 4096
    for(cycle<-0 until cycles) {
      val reset=cycle<2 || cycle==cycles/2
      val ready=cycle%97>=13 && cycle%7!=0
      inputs=Map("clock"->BigInt(0),"reset"->BigInt(if(reset)1 else 0),"io_ready"->BigInt(if(ready)1 else 0))
      memo=Map.empty
      val valid=eval(drivers("io_valid"))
      val mask=ratios.indices.map(i=>eval(drivers(s"io_bits_$i"))<<i).foldLeft(BigInt(0))(_|_)
      state=regs.map(r=>r.name->(if(eval(r.reset)!=0)eval(r.init) else eval(drivers(r.name)))).toMap
      println(s"TOKEN $name $cycle $valid $mask")
    }
  }
}
