// See LICENSE for license details.
package midas.widgets
import chisel3._
import chisel3.util._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import firesim.lib.bridgeutils.SimUtils.TargetRVPortType
import firrtl.annotations.{ModuleTarget,JsonProtocol,NoTargetAnnotation,HasSerializationHints}
import midas.passes.fame.{FAMEChannelConnectionAnnotation,PipeChannel}
import freechips.rocketchip.diplomacy.{LazyModule,LazyModuleImp}
import midas.core.{TargetChannelIO,HostPortIOConnectChannels2Port}
import midas.targetutils.PerfCounterOps
import org.chipsalliance.cde.config.Parameters
/** Calls the unchanged production HostPortIOConnectChannels2Port on two actual
  * CSV widgets. Token stimulus carries the target schedule; ratios are retained
  * separately, and this test does not prove clock-generator phase or F2 boot.
  */
object AutoCounterBridgeBindingOracle extends App {
  require(args.length==1,"output-directory")
  val output=new java.io.File(args(0));require(output.mkdirs()||output.isDirectory)
  implicit val p:Parameters=new midas.F2Config().toInstance
  val fields=Seq("trigger","globalReset","event0","event1")
  val key=AutoCounterParameters(Seq(
    EventMetadata("event0","count","fixture event",64,PerfCounterOps.Accumulate),
    EventMetadata("event1","value","fixture event",64,PerfCounterOps.Identity)),"trigger","globalReset")
  val bridgeAnnos=(0 until 2).map(i=>BridgeIOAnnotation(
    ModuleTarget("Top","Top").ref(s"autocounter$i"),
    fields.map(n=>n->s"g${i}_$n").toMap,
    Some(RationalClock(s"domain$i",1,if(i==0)1 else 3)),
    classOf[AutoCounterBridgeModule].getName,Some(key)))
  val annotations=(0 until 2).flatMap(i=>Seq(bridgeAnnos(i))++fields.map(n=>
    FAMEChannelConnectionAnnotation(s"g${i}_$n",PipeChannel(0),Some(ModuleTarget("Top","Top").ref(s"domain$i")),
      Some(Seq(ModuleTarget("Top","Top").ref(s"token${i}_$n").field("bits"))),None)))
  case class ConstructorHints() extends NoTargetAnnotation with HasSerializationHints {
    def typeHints=Seq(classOf[EventMetadata],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  class Bound extends LazyModule {
    val widgets=(0 until 2).map(i=>LazyModule(new AutoCounterBridgeModule(key)(p.alterPartial{
      case TargetClockInfo=>Some(RationalClock(s"domain$i",1,if(i==0)1 else 3))})))
    lazy val module=new LazyModuleImp(this) {
      val ctrl0=IO(Flipped(WidgetMMIO())).suggestName("ctrl0");val ctrl1=IO(Flipped(WidgetMMIO())).suggestName("ctrl1")
      val tokens0=fields.zipWithIndex.map{case(n,i)=>val t=IO(Flipped(Decoupled(UInt((if(i<2)1 else 64).W))));t.suggestName("w0_"+n);t}
      val tokens1=fields.zipWithIndex.map{case(n,i)=>val t=IO(Flipped(Decoupled(UInt((if(i<2)1 else 64).W))));t.suggestName("w1_"+n);t}
      for(i<-0 until 2) {
        val w=widgets(i).module
        w.io.ctrl <> (if(i==0)ctrl0 else ctrl1)
        val anno=bridgeAnnos(i)
        val channels=new TargetChannelIO {
          def wireOutputPortMap:Map[String,ReadyValidIO[Data]]=fields.zip(if(i==0)tokens0 else tokens1).map{case(n,t)=>s"g${i}_$n"->t.asInstanceOf[ReadyValidIO[Data]]}.toMap
          def wireInputPortMap:Map[String,ReadyValidIO[Data]]=Map.empty
          def rvOutputPortMap:Map[String,TargetRVPortType]=Map.empty
          def rvInputPortMap:Map[String,TargetRVPortType]=Map.empty
          def clockElement:(String,DecoupledIO[Data])=throw new UnsupportedOperationException("CSV does not consume clock-generator channels")
        }
        HostPortIOConnectChannels2Port(w.hPort,anno,channels)
      }
    }
  }
  val owner=LazyModule(new Bound)
  val firrtl=(new ChiselStage).emitChirrtl(owner.module,Array("--target-dir",output.getPath))
  val out=new java.io.PrintWriter(new java.io.File(output,"bound.fir"));try out.write(firrtl) finally out.close()
  val annoOut=new java.io.PrintWriter(new java.io.File(output,"input.anno.json"));try annoOut.write(JsonProtocol.serialize(annotations :+ ConstructorHints())) finally annoOut.close()
  owner.widgets.zipWithIndex.foreach {case(w,i)=>
    val header=new StringBuilder;w.module.genHeader(BigInt(i*64),Map.empty,header)
    val h=new java.io.PrintWriter(new java.io.File(output,s"header-$i.h"));try h.write(header.toString) finally h.close()
  }
  println("PASS unchanged SFC CSV widgets and HostPortIOConnectChannels2Port two-widget oracle")
}
