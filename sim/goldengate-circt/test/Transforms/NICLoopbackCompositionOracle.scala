// See LICENSE for license details.
package midas.widgets
import chisel3._
import chisel3.util._
import chisel3.stage.ChiselStage
import firesim.lib.bridgeutils.BridgeIOAnnotation
import firesim.lib.bridgeutils.SimUtils.TargetRVPortType
import firrtl.annotations.ModuleTarget
import freechips.rocketchip.diplomacy.{LazyModule,LazyModuleImp,IdRange}
import freechips.rocketchip.amba.axi4._
import firechip.goldengateimplementations.{SimpleNICBridgeModule,LoopbackNIC}
import midas.core._
import org.chipsalliance.cde.config.Parameters
/** Actual unchanged LoopbackNIC widgets, shared HasWidgets global control and
  * CPUManagedStreamEngine with F2's full independent AXI4Buffer transport.
  * Pauser payloads are intentionally unobserved (usePauser=false boundary),
  * while their valid/ready channels remain visible. The exact emitted FIRRTL
  * retains the known SFC unassigned pauser outputs and must remain preserved.
  */
object NICLoopbackCompositionOracle extends App {
  require(args.length==1,"output-directory")
  val output=new java.io.File(args(0));require(output.mkdirs()||output.isDirectory)
  implicit val p:Parameters=new midas.F2Config().toInstance.alterPartial {
    case LoopbackNIC=>true
    case ToCPUStreamAllocatorKey=>new StreamAllocator
    case FromCPUStreamAllocatorKey=>new StreamAllocator
  }
  val names=Seq("nic_out_bits_data","nic_out_bits_keep","nic_out_bits_last","nic_out_valid",
    "nic_in_bits_data","nic_in_bits_keep","nic_in_bits_last","nic_in_valid","nic_macAddr",
    "nic_rlimit_inc","nic_rlimit_period","nic_rlimit_size","nic_pauser_threshold","nic_pauser_quanta","nic_pauser_refresh")
  val widths=Seq(64,8,1,1,64,8,1,1,48,8,8,8,16,16,16)
  val annotations=(0 until 2).map(i=>BridgeIOAnnotation(ModuleTarget("Top","Top").ref(s"nic$i"),names.map(n=>n->s"w${i}_$n").toMap,
    None,classOf[SimpleNICBridgeModule].getName,None))
  class Bound extends LazyModule with HasWidgets {
    val nics=(0 until 2).map(_=>addWidget(new SimpleNICBridgeModule))
    val engine=addWidget(new CPUManagedStreamEngine(p,StreamEngineParameters(nics.map(_.streamSourceParams),nics.map(_.streamSinkParams))))
    val cpuParams=p(CPUManagedAXI4Key).get
    val cpuNode=AXI4MasterNode(Seq(AXI4MasterPortParameters(masters=Seq(AXI4MasterParameters(
      name="cpu-managed-axi4",id=IdRange(0,1<<cpuParams.idBits),aligned=false,maxFlight=cpuParams.maxFlight)))))
    engine.cpuManagedAXI4NodeOpt.get := AXI4Buffer() := cpuNode
    lazy val module=new LazyModuleImp(this) {
      val ctrl=IO(Flipped(WidgetMMIO())).suggestName("ctrl");genCtrlIO(ctrl)
      val cpu=IO(Flipped(AXI4Bundle(cpuParams.axi4BundleParams))).suggestName("cpu_managed_axi4");cpuNode.out.head._1<>cpu
      for(i<-0 until 2) {
        engine.streamsToHostCPU(i)<>nics(i).streamEnq
        nics(i).streamDeq<>engine.streamsFromHostCPU(i)
        val tokens=names.indices.map{j=>
          if(j<12) {
            val t=if(j<4)IO(Flipped(Decoupled(UInt(widths(j).W))))else IO(Decoupled(UInt(widths(j).W)))
            t.suggestName(s"w${i}_p$j");t
          } else {
            val t=Wire(Decoupled(UInt(widths(j).W)))
            t.ready:=IO(Input(Bool())).suggestName(s"w${i}_p${j}_ready")
            IO(Output(Bool())).suggestName(s"w${i}_p${j}_valid"):=t.valid
            t
          }
        }
        val channels=new TargetChannelIO {
          def wireOutputPortMap=names.take(4).zip(tokens.take(4)).map{case(n,t)=>s"w${i}_$n"->t.asInstanceOf[ReadyValidIO[Data]]}.toMap
          def wireInputPortMap=names.drop(4).zip(tokens.drop(4)).map{case(n,t)=>s"w${i}_$n"->t.asInstanceOf[ReadyValidIO[Data]]}.toMap
          def rvOutputPortMap:Map[String,TargetRVPortType]=Map.empty
          def rvInputPortMap:Map[String,TargetRVPortType]=Map.empty
          def clockElement:(String,DecoupledIO[Data])=throw new UnsupportedOperationException("no clock generator at this explicit token seam")
        }
        HostPortIOConnectChannels2Port(nics(i).module.hPort,annotations(i),channels)
      }
    }
  }
  val owner=LazyModule(new Bound)
  def write(name:String,text:String):Unit={val out=new java.io.PrintWriter(new java.io.File(output,name));try out.write(text)finally out.close()}
  write("bound.fir",(new ChiselStage).emitChirrtl(owner.module,Array("--target-dir",output.getPath)))
  val header=new StringBuilder;owner.genWidgetHeaders(header,Map.empty);write("control.h",header.toString)
  val peer=new StringBuilder;owner.genWidgetPeerToPeerAddrMap(peer);write("peer.yaml",peer.toString)
  println("PASS unchanged SFC complete two LoopbackNIC/shared MMIO/buffered CPU composition; retained pauser initialization defect")
}
