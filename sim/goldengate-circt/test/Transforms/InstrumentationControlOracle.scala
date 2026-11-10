// See LICENSE for license details.
package midas.widgets
import chisel3._
import chisel3.util._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.{BridgeIOAnnotation,RationalClock}
import firesim.lib.bridgeutils.SimUtils.TargetRVPortType
import firrtl.annotations.{ModuleTarget,JsonProtocol,NoTargetAnnotation,HasSerializationHints}
import midas.passes.fame.{FAMEChannelConnectionAnnotation,PipeChannel}
import freechips.rocketchip.diplomacy.{LazyModule,LazyModuleImp,IdRange}
import freechips.rocketchip.amba.axi4._
import midas.core._
import midas.targetutils.PerfCounterOps
import org.chipsalliance.cde.config.Parameters
/** Actual unchanged CSV, enabled Print, shared HasWidgets control and buffered
  * CPUManagedStreamEngine. External tokens are an explicit verification seam;
  * generated target clocks, original platform bridges and Linux are separate.
  */
object InstrumentationControlOracle extends App {
  require(args.length==1,"output-directory")
  val output=new java.io.File(args(0));require(output.mkdirs()||output.isDirectory)
  implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
    case ToCPUStreamAllocatorKey=>new StreamAllocator
    case FromCPUStreamAllocatorKey=>new StreamAllocator
  }
  val csvFields=Seq("trigger","globalReset","event0","event1")
  val key=AutoCounterParameters(Seq(
    EventMetadata("event0","count","fixture event",64,PerfCounterOps.Accumulate),
    EventMetadata("event1","value","fixture event",64,PerfCounterOps.Identity)),"trigger","globalReset")
  val printFields=Seq("reset","record0_enable","record0_argument","record1_enable","record1_argument")
  val printKey=PrintBridgeParameters("reset",Seq(
    PrintPort("record0",Seq("enable"->"UInt<1>","argument"->"SInt<5>"),"signed=%d\n"),
    PrintPort("record1",Seq("enable"->"UInt<1>","argument"->"UInt<3>"),"extra=%x\n")))
  val csvAnnos=(0 until 2).map(i=>BridgeIOAnnotation(ModuleTarget("Top","Top").ref(s"autocounter$i"),
    csvFields.map(n=>n->s"g${i}_$n").toMap,Some(RationalClock(s"domain$i",1,if(i==0)1 else 3)),classOf[AutoCounterBridgeModule].getName,Some(key)))
  val printAnno=BridgeIOAnnotation(ModuleTarget("Top","Top").ref("synthesizedPrintf"),printFields.map(n=>n->s"p_$n").toMap,
    Some(RationalClock("domain1",1,3)),classOf[PrintBridgeModule].getName,Some(printKey))
  val annotations=(0 until 2).flatMap(i=>Seq(csvAnnos(i))++csvFields.map(n=>FAMEChannelConnectionAnnotation(s"g${i}_$n",PipeChannel(0),
    Some(ModuleTarget("Top","Top").ref(s"domain$i")),Some(Seq(ModuleTarget("Top","Top").ref(s"token${i}_$n").field("bits"))),None))) ++
    Seq(printAnno) ++ printFields.map(n=>FAMEChannelConnectionAnnotation(s"p_$n",PipeChannel(0),Some(ModuleTarget("Top","Top").ref("domain1")),
      Some(Seq(ModuleTarget("Top","Top").ref(s"printToken_$n").field("bits"))),None))
  case class ConstructorHints() extends NoTargetAnnotation with HasSerializationHints {
    def typeHints=Seq(classOf[EventMetadata],classOf[PrintPort],PerfCounterOps.Accumulate.getClass,PerfCounterOps.Identity.getClass)
  }
  class Bound extends LazyModule with HasWidgets {
    val csv=(0 until 2).map(i=>addWidget(new AutoCounterBridgeModule(key)(p.alterPartial{
      case TargetClockInfo=>Some(RationalClock(s"domain$i",1,if(i==0)1 else 3))})))
    val prints=addWidget(new PrintBridgeModule(printKey)(p.alterPartial{case TargetClockInfo=>Some(RationalClock("domain1",1,3))}))
    val engine=addWidget(new CPUManagedStreamEngine(p,StreamEngineParameters(Seq(prints.streamSourceParams),Seq.empty)))
    val cpuParams=p(CPUManagedAXI4Key).get
    val cpuNode=AXI4MasterNode(Seq(AXI4MasterPortParameters(masters=Seq(AXI4MasterParameters(
      name="cpu-managed-axi4",id=IdRange(0,1<<cpuParams.idBits),aligned=false,maxFlight=cpuParams.maxFlight)))))
    engine.cpuManagedAXI4NodeOpt.get := AXI4Buffer() := cpuNode
    lazy val module=new LazyModuleImp(this) {
      val ctrl=IO(Flipped(WidgetMMIO())).suggestName("ctrl");genCtrlIO(ctrl)
      val cpu=IO(Flipped(AXI4Bundle(cpuParams.axi4BundleParams))).suggestName("cpu_managed_axi4")
      cpuNode.out.head._1 <> cpu
      engine.streamsToHostCPU.head <> prints.streamEnq
      def channels(map:Map[String,ReadyValidIO[Data]])=new TargetChannelIO {
        def wireOutputPortMap=map
        def wireInputPortMap:Map[String,ReadyValidIO[Data]]=Map.empty
        def rvOutputPortMap:Map[String,TargetRVPortType]=Map.empty
        def rvInputPortMap:Map[String,TargetRVPortType]=Map.empty
        def clockElement:(String,DecoupledIO[Data])=throw new UnsupportedOperationException("no clock-generator input at this seam")
      }
      for(i<-0 until 2) {
        val tokens=csvFields.zipWithIndex.map{case(n,j)=>val t=IO(Flipped(Decoupled(UInt((if(j<2)1 else 64).W))));t.suggestName(s"w${i}_$n");t}
        HostPortIOConnectChannels2Port(csv(i).module.hPort,csvAnnos(i),channels(csvFields.zip(tokens).map{case(n,t)=>s"g${i}_$n"->t.asInstanceOf[ReadyValidIO[Data]]}.toMap))
      }
      val ptokens=printFields.zipWithIndex.map{case(n,j)=>
        val t=IO(Flipped(Decoupled(if(j==2)SInt(5.W) else UInt((if(j==4)3 else 1).W))));t.suggestName(s"p_$n");t}
      HostPortIOConnectChannels2Port(prints.module.hPort,printAnno,channels(printFields.zip(ptokens).map{case(n,t)=>s"p_$n"->t.asInstanceOf[ReadyValidIO[Data]]}.toMap))
    }
  }
  val owner=LazyModule(new Bound)
  val firrtl=(new ChiselStage).emitChirrtl(owner.module,Array("--target-dir",output.getPath))
  def write(name:String,text:String):Unit={val out=new java.io.PrintWriter(new java.io.File(output,name));try out.write(text)finally out.close()}
  write("bound.fir",firrtl);write("input.anno.json",JsonProtocol.serialize(annotations:+ConstructorHints()))
  val header=new StringBuilder;owner.genWidgetHeaders(header,Map.empty);write("control.h",header.toString)
  val peer=new StringBuilder;owner.genWidgetPeerToPeerAddrMap(peer);write("peer.yaml",peer.toString)
  println("PASS unchanged SFC complete enabled CSV + Print + buffered CPU shared control oracle")
}
